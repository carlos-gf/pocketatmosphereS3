#include "buzz.h"
#include "board_cores3.h"
#include "haptic.h"
#include "ring.h"
#include <math.h>

// ---------------------------------------------------------------------------
// VOZ, version CoreS3.
//
// En la 1.69 esto era un piezo movido por LEDC: una onda cuadrada, un tono cada
// vez, y la envolvente hecha variando el ciclo de trabajo porque no habia otra
// forma de dar amplitud. Sonaba a electrodomestico y la nota base tenia que
// quedarse por encima de ~460 Hz o el piezo no movia aire, lo que invertia el
// "mas profundo = mas grave" en cuatro de los seis campos.
//
// Aqui hay un altavoz de verdad con amplificador I2S, asi que la restriccion
// desaparece: la envolvente es amplitud real y el tono puede bajar donde tiene
// que bajar. Lo que NO cambia es la decision de diseno: la voz es PUNTUACION,
// no fondo. Eventos escasos, separados por huecos exponenciales. Un zumbido
// continuo convertiria el campo en una maquina.
// ---------------------------------------------------------------------------

static bool enabled = true;
static uint32_t nextEvent = 0;

// ruido barato y repetible, sin depender de random()
static uint32_t rngState = 0x1234567u;
static float frand() {
  rngState = rngState * 1664525u + 1013904223u;
  return (float)((rngState >> 8) & 0xFFFF) / 65535.0f;
}

// Dos canales: la voz del campo y las confirmaciones de interfaz. Separarlos
// evita que un evento largo se corte al tocar una tecla, y sobre todo evita
// tocar el volumen maestro en cada evento -que era lo que dejaba mudo todo lo
// siguiente-.
#define CH_VOICE 0
#define CH_UI    1

void buzzBegin() {
  M5.Speaker.begin();          // configuracion por defecto: la de la placa
  M5.Speaker.setVolume(180);   // maestro alto; la dinamica se hace por canal
  M5.Speaker.setChannelVolume(CH_UI, 90);
}

void buzzSetEnabled(bool on) {
  enabled = on;
  if (!on) M5.Speaker.stop();
}

void buzzPing(uint16_t hz, uint16_t ms) {
  if (!enabled) return;
  M5.Speaker.tone(hz, ms, CH_UI, true);
  hapticPulse(0.22f, 40);   // confirmaciones de interfaz: un toque, no un aviso
}

void buzzSilence() { M5.Speaker.stop(); }

// Diagnostico por USB: dice lo que el altavoz cree de si mismo y recorre la
// banda util, para no volver a discutir a ciegas si "el sonido no va".
void buzzSelfTest(void (*say)(const char *)) {
  char b[96];
  snprintf(b, sizeof b, "speaker enabled=%d volume=%d",
           (int)M5.Speaker.isEnabled(), (int)M5.Speaker.getVolume());
  say(b);
  static const int hzs[] = { 220, 380, 500, 700, 1000, 1500, 2000 };
  for (int i = 0; i < 7; i++) {
    snprintf(b, sizeof b, "  tone %d Hz", hzs[i]);
    say(b);
    M5.Speaker.setChannelVolume(CH_VOICE, 200);
    M5.Speaker.tone(hzs[i], 400, CH_VOICE, true);
    delay(500);
  }
  say("if only the higher ones were audible, that is the speaker, not the code");
}

void buzzUpdate(uint32_t now, uint16_t centrePitch, uint8_t spreadSemitones,
                uint8_t ratePerMin, float agitation, float depth) {
  if (!enabled) return;
  if (!nextEvent) nextEvent = now + 1500;
  if (now < nextEvent) return;

  // La agitacion del IMU acelera los eventos; la reduccion los alarga y los baja.
  // Con 2..8 eventos por minuto y huecos exponenciales, se podia estar un minuto
  // entero sin oir nada y concluir que no funcionaba. Sigue siendo puntuacion,
  // pero lo bastante presente como para saber que esta vivo.
  float rate = ratePerMin * 2.6f * (1.0f + 2.2f * agitation);
  if (rate < 2.0f) rate = 2.0f;
  float meanGap = 60000.0f / rate;
  // huecos exponenciales: irregulares, nunca un metronomo
  float u = frand();
  if (u < 0.0001f) u = 0.0001f;
  nextEvent = now + (uint32_t)(-logf(u) * meanGap);

  // REGISTRO. Dije que con altavoz desaparecia la restriccion del piezo. Es
  // falso a medias: el altavoz de la CoreS3 es de 1 W y del tamano de una
  // moneda, y por debajo de unos 380 Hz no mueve aire. Los tonos de 120..350 Hz
  // que salian antes existian, pero no se oian: de ahi el "suena raro y muy
  // bajito". La direccion "mas reducido = mas grave" se conserva, pero DENTRO
  // de la banda donde este altavoz existe.
  float semis = (frand() - 0.5f) * 2.0f * spreadSemitones;
  semis -= depth * 5.0f;
  float hz = centrePitch * 2.2f * powf(2.0f, semis / 12.0f);
  if (hz < 380.0f) hz = 380.0f;
  if (hz > 1900.0f) hz = 1900.0f;

  // Duracion: los eventos profundos son largos y blandos; los superficiales,
  // cortos y nitidos.
  uint16_t ms = (uint16_t)(90.0f + 520.0f * depth + 140.0f * frand());

  // Volumen por evento: la profundidad tambien apaga.
  uint8_t vol = (uint8_t)(120.0f - 55.0f * depth + 40.0f * agitation);
  M5.Speaker.setChannelVolume(CH_VOICE, vol);
  M5.Speaker.tone(hz, ms, CH_VOICE, true);

  // El mismo acontecimiento, en la mano. Mas profundo = mas largo y mas debil:
  // en el extremo reducido el aparato casi no se anuncia, solo respira.
  hapticPulse(0.30f + 0.45f * agitation - 0.15f * depth, (uint16_t)(ms * 0.55f));
  ringPulse(0.45f + 0.4f * agitation);
}
