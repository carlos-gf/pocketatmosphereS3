#include "haptic.h"
#include "board_cores3.h"

// 20 kHz: por encima de lo audible, asi que el motor no chirria ademas de vibrar.
#define HAPTIC_FREQ 20000
#define HAPTIC_BITS 10
#define HAPTIC_MAX  1023

// La patada de arranque. Un motor pequeno no rompe la inercia por debajo de
// ~60% de duty; despues se sostiene con mucho menos.
#define KICK_MS   18
#define KICK_DUTY 1000

static bool enabled = true;
static bool ready = false;
static uint32_t pulseEnd = 0, kickEnd = 0;
static int holdDuty = 0;

static inline void write(int duty) {
  if (!ready) return;
  if (duty < 0) duty = 0;
  if (duty > HAPTIC_MAX) duty = HAPTIC_MAX;
  ledcWrite(HAPTIC_PIN, duty);
}

void hapticBegin() {
  ready = ledcAttach(HAPTIC_PIN, HAPTIC_FREQ, HAPTIC_BITS);
  write(0);
}

void hapticSetEnabled(bool on) {
  enabled = on;
  if (!on) hapticStop();
}

void hapticStop() {
  pulseEnd = kickEnd = 0;
  holdDuty = 0;
  write(0);
}

void hapticPulse(float level, uint16_t ms) {
  if (!enabled || !ready) return;
  if (level <= 0.0f) return;
  if (level > 1.0f) level = 1.0f;
  // El sostenido util vive entre el 18% y el 55%: por encima ya no es una
  // presencia, es un telefono sonando.
  holdDuty = (int)((0.18f + 0.37f * level) * HAPTIC_MAX);
  uint32_t now = millis();
  kickEnd = now + KICK_MS;
  pulseEnd = now + (ms < KICK_MS + 10 ? KICK_MS + 10 : ms);
  write(KICK_DUTY);
}

void hapticUpdate(uint32_t now) {
  if (!ready) return;
  if (!pulseEnd) return;
  if (now >= pulseEnd) {
    hapticStop();
    return;
  }
  if (now >= kickEnd) {
    // caida suave hasta el final del pulso: se va, no se corta
    uint32_t left = pulseEnd - now, span = pulseEnd - kickEnd;
    float f = span ? (float)left / (float)span : 0.0f;
    write((int)(holdDuty * (0.35f + 0.65f * f)));
  }
}
