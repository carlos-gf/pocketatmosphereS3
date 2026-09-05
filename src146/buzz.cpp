#include "buzz.h"
#include "board_ws146.h"
#include <math.h>
#include <driver/i2s_std.h>

// ---------------------------------------------------------------------------
// VOZ, version 1.46.
//
// Aqui no hay zumbador ni libreria que lo resuelva: hay un DAC PCM5101 colgado
// de un I2S propio. Asi que la voz se sintetiza a mano -una senoide con
// envolvente- y se escribe al bus. Es mas trabajo y es mejor: la envolvente es
// amplitud de verdad, no un truco de ciclo de trabajo, y el registro ya no
// depende de lo que un piezo sea capaz de mover.
//
// Se mantiene la decision de siempre: la voz es PUNTUACION, no fondo. Eventos
// escasos con huecos exponenciales. Un zumbido continuo convertiria el campo en
// una maquina.
// ---------------------------------------------------------------------------

#define SR 24000

static bool enabled = true;
static uint32_t nextEvent = 0;
static i2s_chan_handle_t txChan = nullptr;

static uint32_t rngState = 0x1234567u;
static float frand() {
  rngState = rngState * 1664525u + 1013904223u;
  return (float)((rngState >> 8) & 0xFFFF) / 65535.0f;
}

void buzzBegin() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&cc, &txChan, nullptr) != ESP_OK) { txChan = nullptr; return; }
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)I2S_SPK_BCLK,
          .ws = (gpio_num_t)I2S_SPK_LRCK,
          .dout = (gpio_num_t)I2S_SPK_DOUT,
          .din = I2S_GPIO_UNUSED,
          .invert_flags = {false, false, false},
      },
  };
  if (i2s_channel_init_std_mode(txChan, &sc) != ESP_OK) { txChan = nullptr; return; }
  i2s_channel_enable(txChan);
}

void buzzSetEnabled(bool on) { enabled = on; }
void buzzSilence() {}

// Una nota con ataque y caida suaves, escrita en trozos para no bloquear mas de
// unos milisegundos seguidos.
static void playTone(float hz, uint16_t ms, float gain) {
  if (!txChan || !enabled) return;
  const int CH = 256;
  static int16_t buf[CH * 2];
  uint32_t total = (uint32_t)((uint64_t)SR * ms / 1000);
  float phase = 0.0f, step = 2.0f * (float)M_PI * hz / SR;
  uint32_t done = 0;
  uint32_t att = total / 6, rel = total / 3;
  while (done < total) {
    int n = (int)((total - done) < CH ? (total - done) : CH);
    for (int i = 0; i < n; i++) {
      uint32_t k = done + i;
      float env = 1.0f;
      if (k < att) env = (float)k / att;
      else if (k > total - rel) env = (float)(total - k) / rel;
      env = env * env * (3.0f - 2.0f * env);            // entrada y salida suaves
      int16_t v = (int16_t)(sinf(phase) * 26000.0f * gain * env);
      phase += step;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      buf[i * 2] = v;
      buf[i * 2 + 1] = v;
    }
    size_t wrote = 0;
    i2s_channel_write(txChan, buf, n * 4, &wrote, 40);
    done += n;
  }
}

void buzzPing(uint16_t hz, uint16_t ms) { playTone(hz, ms, 0.42f); }

void buzzSelfTest(void (*say)(const char *)) {
  char b[80];
  static const int hzs[] = { 220, 380, 500, 700, 1000, 1500, 2000 };
  for (int i = 0; i < 7; i++) {
    snprintf(b, sizeof b, "  tone %d Hz", hzs[i]);
    say(b);
    playTone(hzs[i], 350, 0.9f);
    delay(120);
  }
  say("if only the higher ones were audible, that is the speaker, not the code");
}

void buzzUpdate(uint32_t now, uint16_t centrePitch, uint8_t spreadSemitones,
                uint8_t ratePerMin, float agitation, float depth) {
  if (!enabled || !txChan) return;
  if (!nextEvent) nextEvent = now + 1500;
  if (now < nextEvent) return;

  float rate = ratePerMin * 2.6f * (1.0f + 2.2f * agitation);
  if (rate < 2.0f) rate = 2.0f;
  float u = frand();
  if (u < 0.0001f) u = 0.0001f;
  nextEvent = now + (uint32_t)(-logf(u) * (60000.0f / rate));

  // Con altavoz de verdad la banda util baja bastante mas que en la CoreS3,
  // pero sigue sin ser infinita: por debajo de ~250 Hz este tamano no rinde.
  float semis = (frand() - 0.5f) * 2.0f * spreadSemitones - depth * 6.0f;
  float hz = centrePitch * 1.7f * powf(2.0f, semis / 12.0f);
  if (hz < 250.0f) hz = 250.0f;
  if (hz > 1900.0f) hz = 1900.0f;

  uint16_t ms = (uint16_t)(120.0f + 520.0f * depth + 140.0f * frand());
  playTone(hz, ms, 0.30f + 0.35f * (1.0f - depth) + 0.25f * agitation);
}
