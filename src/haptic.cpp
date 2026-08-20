#include "haptic.h"
#include "board_cores3.h"
#include <math.h>

// 20 kHz: por encima de lo audible, asi que el motor no chirria ademas de vibrar.
#define HAPTIC_FREQ 20000
#define HAPTIC_BITS 10
#define HAPTIC_MAX  1023

// ---------------------------------------------------------------------------
// La primera version era un aviso: patada a plena potencia y corte seco. En la
// mano eso no es una presencia, es un telefono. Un acontecimiento del campo
// tiene que ENTRAR y SALIR, no aparecer y desaparecer.
//
// Asi que la envolvente es ataque - sostenido - caida, calculada a 100 Hz, con
// la patada reducida al minimo necesario para vencer la inercia (y solo si el
// motor estaba parado). El nivel sostenido util quedo mucho mas bajo que antes.
//
// Y hay una diferencia entre estar en la mano y estar sobre la mesa: sostenido
// por alguien, el IMU nunca da cero. Cuando el aparato se sabe sostenido, los
// pulsos son mas largos y mas suaves -pueden serlo, porque hay alguien-. Sobre
// la mesa se acortan hasta casi desaparecer: no hay a quien avisar.
// ---------------------------------------------------------------------------

#define TICK_MS      10
#define KICK_MS      12
#define KICK_DUTY    820

static bool enabled = true;
static bool ready = false;
static bool held = false;

static uint32_t t0 = 0, tAttack = 0, tRelease = 0, tEnd = 0, lastTick = 0;
static float peak = 0.0f, cur = 0.0f;
static bool kicking = false;

static inline void write(float level) {
  if (!ready) return;
  int duty = (int)(level * HAPTIC_MAX + 0.5f);
  if (duty < 0) duty = 0;
  if (duty > HAPTIC_MAX) duty = HAPTIC_MAX;
  ledcWrite(HAPTIC_PIN, duty);
}

void hapticBegin() {
  ready = ledcAttach(HAPTIC_PIN, HAPTIC_FREQ, HAPTIC_BITS);
  write(0.0f);
}

void hapticSetEnabled(bool on) {
  enabled = on;
  if (!on) hapticStop();
}

void hapticSetHeld(bool h) { held = h; }

void hapticStop() {
  t0 = tEnd = 0;
  cur = peak = 0.0f;
  kicking = false;
  write(0.0f);
}

void hapticPulse(float level, uint16_t ms) {
  if (!enabled || !ready || level <= 0.0f) return;
  if (level > 1.0f) level = 1.0f;

  // Sostenido util: 8%..34%. Por encima ya no acompana, interrumpe.
  peak = 0.08f + 0.26f * level;

  // En la mano puede durar mas y entrar mas despacio. En la mesa, lo contrario.
  float stretch = held ? 1.9f : 0.7f;
  uint32_t dur = (uint32_t)(ms * stretch);
  if (dur < 120) dur = 120;
  if (dur > 1600) dur = 1600;

  uint32_t now = millis();
  t0 = now;
  tAttack = now + (uint32_t)(dur * (held ? 0.35f : 0.18f));   // entra
  tRelease = now + (uint32_t)(dur * 0.55f);                   // se sostiene
  tEnd = now + dur;                                          // y se va
  // La patada solo hace falta si el motor esta parado; si ya gira, estorba.
  kicking = (cur < 0.02f);
  if (kicking) write(KICK_DUTY / (float)HAPTIC_MAX);
}

void hapticUpdate(uint32_t now) {
  if (!ready || !tEnd) return;
  if (now - lastTick < TICK_MS) return;
  lastTick = now;

  if (kicking && now - t0 >= KICK_MS) kicking = false;
  if (kicking) return;

  if (now >= tEnd) {
    hapticStop();
    return;
  }

  float target;
  if (now < tAttack) {
    float f = (float)(now - t0) / (float)(tAttack - t0 ? tAttack - t0 : 1);
    target = peak * f * f * (3.0f - 2.0f * f);       // entrada suave
  } else if (now < tRelease) {
    target = peak;
  } else {
    float f = (float)(tEnd - now) / (float)(tEnd - tRelease ? tEnd - tRelease : 1);
    target = peak * f * f * (3.0f - 2.0f * f);       // salida suave
  }

  // filtro de un polo: aunque el objetivo salte, el motor no da tirones
  cur += (target - cur) * 0.35f;
  write(cur);
}
