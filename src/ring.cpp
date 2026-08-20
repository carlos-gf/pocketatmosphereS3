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
static uint8_t baseR = 0, baseG = 0, baseB = 0;
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

void ringSetField(uint16_t c, float brightness) {
  // RGB565 -> 8 bits por canal
  baseR = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
  baseG = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
  baseB = (uint8_t)((c & 0x1F) * 255 / 31);
  baseLevel = brightness < 0 ? 0 : (brightness > 1 ? 1 : brightness);
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

  uint8_t r = (uint8_t)(baseR * lvl);
  uint8_t g = (uint8_t)(baseG * lvl);
  uint8_t b = (uint8_t)(baseB * lvl);
  for (uint8_t i = 0; i < nLeds; i++) strip->setPixelColor(i, r, g, b);
  strip->show();
}
