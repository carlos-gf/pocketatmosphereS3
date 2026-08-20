#include "ring.h"
#include "board_cores3.h"
#include <Adafruit_NeoPixel.h>

// Tope duro de brillo. Por encima de esto deja de ser atmosfera y empieza a ser
// una linterna -y ademas el bus de 5 V del Grove no lo sostiene-.
#define RING_CEIL 0.16f

static Adafruit_NeoPixel *strip = nullptr;
static uint8_t nLeds = 0;
static uint8_t curPin = LED_RING_PIN;
static bool enabled = true;
#define RING_MAX 24
// Objetivo y valor actual por LED. El anillo NUNCA salta al objetivo: se
// arrastra hacia el. Un cambio instantaneo se lee como un aviso; uno lento se
// lee como luz.
static float tgtR[RING_MAX], tgtG[RING_MAX], tgtB[RING_MAX];
static float curR[RING_MAX], curG[RING_MAX], curB[RING_MAX];
static float baseLevel = 0.0f;
static float pulseLevel = 0.0f;
static uint32_t lastUpdate = 0;

void ringBegin(uint8_t count) { ringRewire(LED_RING_PIN, count); }

uint8_t ringPin() { return curPin; }

void ringRewire(uint8_t pin, uint8_t count) {
  if (strip) { strip->clear(); strip->show(); delete strip; }
  curPin = pin;
  nLeds = count;
  strip = new Adafruit_NeoPixel(count, pin, NEO_GRB + NEO_KHZ800);
  strip->begin();
  strip->setBrightness(255);
  strip->clear();
  strip->show();
}

// Blanco al tope permitido. Si esto no enciende nada, el problema esta en el
// cable, en la alimentacion o en el pin, no en la logica del campo.
void ringSelfTest(uint16_t ms) {
  if (!strip) return;
  uint8_t v = (uint8_t)(255 * RING_CEIL);
  for (uint8_t i = 0; i < nLeds; i++) strip->setPixelColor(i, v, v, v);
  strip->show();
  delay(ms);
  strip->clear();
  strip->show();
}

void ringSetEnabled(bool on) {
  enabled = on;
  if (!on) ringOff();
}

void ringOff() {
  if (!strip) return;
  strip->clear();
  strip->show();
}

uint8_t ringCount() { return nLeds; }

void ringSetField(uint16_t c, float brightness) {
  uint16_t one[1] = { c };
  ringSetColors(one, 1, brightness);
}

void ringSetColors(const uint16_t *cols, uint8_t n, float brightness) {
  baseLevel = brightness < 0 ? 0 : (brightness > 1 ? 1 : brightness);
  if (!n) return;
  for (uint8_t i = 0; i < nLeds && i < RING_MAX; i++) {
    uint16_t c = cols[n == 1 ? 0 : (i % n)];
    tgtR[i] = ((c >> 11) & 0x1F) * 255.0f / 31.0f;
    tgtG[i] = ((c >> 5) & 0x3F) * 255.0f / 63.0f;
    tgtB[i] = (c & 0x1F) * 255.0f / 31.0f;
  }
}

void ringPulse(float level) {
  if (level < 0) level = 0;
  if (level > 1) level = 1;
  if (level > pulseLevel) pulseLevel = level;
}

void ringUpdate(uint32_t now) {
  if (!strip || !enabled) return;
  if (now - lastUpdate < 33) return;   // 30 Hz basta: esto respira, no parpadea
  float dt = (now - lastUpdate) / 1000.0f;
  lastUpdate = now;

  // el realce se va solo, con una caida lenta: un evento deja rastro
  pulseLevel -= pulseLevel * dt * 1.6f;
  if (pulseLevel < 0.002f) pulseLevel = 0.0f;

  float lvl = (baseLevel + pulseLevel * 0.9f) * RING_CEIL;
  if (lvl > RING_CEIL) lvl = RING_CEIL;

  // Arrastre hacia el objetivo. La constante es deliberadamente lenta: a 30 Hz
  // tarda alrededor de un segundo en llegar, que es el tiempo que separa "se
  // esta moviendo" de "ha cambiado".
  const float k = 0.06f;
  for (uint8_t i = 0; i < nLeds && i < RING_MAX; i++) {
    curR[i] += (tgtR[i] - curR[i]) * k;
    curG[i] += (tgtG[i] - curG[i]) * k;
    curB[i] += (tgtB[i] - curB[i]) * k;
    strip->setPixelColor(i, (uint8_t)(curR[i] * lvl), (uint8_t)(curG[i] * lvl), (uint8_t)(curB[i] * lvl));
  }
  strip->show();
}
