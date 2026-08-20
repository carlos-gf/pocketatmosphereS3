#pragma once
#include <stdint.h>
#include "atmos.h"

// El campo se calcula en una rejilla gruesa y se interpola al panel completo.
// 320x240 con celda de 8 px -> 41x31 muestras de ruido por fotograma.
#define FIELD_W 320
#define FIELD_H 240
#define FIELD_CELL 8
#define FIELD_GW (FIELD_W / FIELD_CELL + 1)
#define FIELD_GH (FIELD_H / FIELD_CELL + 1)

// LovyanGFX (M5Canvas) guarda el sprite de 16 bits con los bytes intercambiados
// respecto a como los escribia Arduino_GFX. Al volcar el campo directamente en
// el bufer hay que darle la vuelta a cada pixel, o los colores salen chillones:
// un gris azulado se convierte en naranja. Lo que se dibuja por la API (texto,
// rectangulos) NO lleva swap: de eso ya se encarga la libreria.
#if defined(ARDUINO) && !defined(FIELD_HOST)
#define FIELD_OUT(c) ((uint16_t)(((uint16_t)(c) >> 8) | ((uint16_t)(c) << 8)))
#else
#define FIELD_OUT(c) (c)
#endif

struct FieldState {
  uint8_t atmos = 0;
  float depth = 0.55f;    // 0 = reduccion leve (mas estructura), 1 = reduccion profunda
  float t = 0.0f;         // tiempo de deriva acumulado (avanza segun el ambiente)
  float agitation = 0.0f; // 0..1, del IMU: quieto asienta el campo, moverlo lo altera
  float daylight = 0.5f;  // 0..1, del RTC: sesga el campo hacia la parte alta o baja
};

// Renderiza el campo completo en un framebuffer RGB565 de FIELD_W*FIELD_H.
void fieldRender(uint16_t *fb, const FieldState &st);

// Renderiza un recuadro pequeno (para la rejilla de la coleccion).
void fieldSwatch(uint16_t *fb, int fbw, int fbh, int x0, int y0, int w, int h, const FieldState &st);

// Color medio del campo con estos ajustes (para barras, texto legible, etc).
uint16_t fieldMidColor(const FieldState &st);
// Devuelve true si el campo es claro, para elegir tinta oscura encima.
bool fieldIsLight(const FieldState &st);

// Muestrea el campo en un ANILLO: n colores repartidos por una circunferencia,
// sacados del mismo ruido y la misma paleta que la pantalla. Asi la luz no es
// una decoracion aparte, es el mismo tiempo atmosferico saliendo por el borde.
void fieldRingColors(const FieldState &st, uint16_t *out, int n);

// Cuantos colores distintos puede producir el campo con esta profundidad.
int fieldLevels(float depth);
