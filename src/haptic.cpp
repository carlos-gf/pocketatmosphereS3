#include "haptic.h"
#include "board_cores3.h"
#include <math.h>

// PWM a 1,2 kHz, no a 20 kHz. A 20 kHz la inductancia del motor limita la
// corriente y a duty bajo no llega par: se queda quieto. A ~1 kHz sigue siendo
// bastante agudo como para no molestar y el motor si responde a duty bajo.
#define HAPTIC_FREQ 1200
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

  // ESTE era el fallo del intento anterior. Al suavizarlo baje el sostenido a
  // 8%..34%, que esta POR DEBAJO del punto en que este motor arranca: la patada
  // inicial lo movia y luego se calaba. Por eso solo se notaban los toques de
  // interfaz -que son casi solo patada- y los eventos del campo, mas largos,
  // no se sentian en absoluto. El sostenido util empieza alrededor del 34%.
  peak = 0.34f + 0.30f * level;

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
