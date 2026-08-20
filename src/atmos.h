#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Un "ambiente" (atmosphere) es, en este aparato, tres cosas:
//
//   1. una DISTRIBUCION DE COLOR   -> 16 entradas, ordenadas de oscuro a claro
//   2. un COMPORTAMIENTO TEMPORAL  -> con que velocidad y grano se mueve
//   3. una PROFUNDIDAD DE REDUCCION-> un solo numero, 0..1, que el beholder mueve
//
// Deliberadamente NO tiene nombre visible. El aparato nunca dice de que lugar
// viene cada campo: nombrarlo es trabajo del que lo sostiene. El lugar de origen
// se guarda aqui solo para poder revelarlo si el ajuste "reveal" esta activo.
// ---------------------------------------------------------------------------

// Dos versiones del aparato desde el mismo codigo:
//   IMAGE_FIELDS 0  campos de ruido con paletas de autor      (v0.1)
//   IMAGE_FIELDS 1  campos destilados de fotografias reales   (v0.2)
#ifndef IMAGE_FIELDS
#define IMAGE_FIELDS 0
#endif

#if IMAGE_FIELDS
#include "images.h"
#define ATMOS_COUNT IMG_COUNT
#else
#define ATMOS_COUNT 6
#endif
#define PAL_SIZE 16

// 0xRRGGBB -> RGB565
#define C24(h) ((uint16_t)((((h) >> 19) & 0x1F) << 11 | (((h) >> 10) & 0x3F) << 5 | (((h) >> 3) & 0x1F)))

struct Atmos {
  const uint32_t *pal24;
  const uint16_t *rgb;     // la fotografia entera, 320x240 RGB565, sin resumir
                           // (el multiconjunto que se permuta; la luminancia se
                           //  deriva al cargar, no se guarda)
  uint16_t driftMs;        // periodo base de deriva (mayor = mas lento)
  uint8_t grain;           // 0..255 cuanto grano de alta frecuencia admite
  uint8_t warmth;          // 0..255 sesgo de temperatura al aplicar la hora
  uint16_t voicePitch;     // Hz centro de los eventos sonoros
  uint8_t voiceSpread;     // semitonos de dispersion
  uint8_t voiceRate;       // eventos por minuto a agitacion cero
  uint8_t tilt;            // 128 = plano; >128 se aclara arriba, <128 se aclara abajo
  const char *source;      // SOLO para el modo revelar; nunca en la pantalla principal
};

// --- I ---------------------------------------------------------------------
static const uint32_t PAL_1[PAL_SIZE] = {
  0x0b1016, 0x121a24, 0x1b2530, 0x26323d, 0x33404b, 0x414e59, 0x4f5d68, 0x5e6d77,
  0x6e7d86, 0x7f8d95, 0x919ea5, 0xa4b0b6, 0xb8c3c8, 0xccd5d9, 0xd9dfe2, 0xf2c98a,
};
// --- II --------------------------------------------------------------------
static const uint32_t PAL_2[PAL_SIZE] = {
  0x070b09, 0x0d1410, 0x131d17, 0x1a271e, 0x223126, 0x2a3b2e, 0x334537, 0x3c5040,
  0x465b4a, 0x516654, 0x5d715f, 0x6b7d6b, 0x7b8a78, 0x8f9c8a, 0xa8b3a1, 0xc6cdbd,
};
// --- III -------------------------------------------------------------------
static const uint32_t PAL_3[PAL_SIZE] = {
  0x150d07, 0x1f140b, 0x2a1c10, 0x362415, 0x432d1b, 0x513722, 0x60422a, 0x705032,
  0x80603c, 0x907047, 0xa08155, 0xb09365, 0xc0a678, 0xd0ba8e, 0xe0cfa8, 0xf0e4c6,
};
// --- IV --------------------------------------------------------------------
static const uint32_t PAL_4[PAL_SIZE] = {
  0x6f7780, 0x7b838c, 0x878f98, 0x939ba4, 0x9fa7b0, 0xabb3bb, 0xb7bfc6, 0xc3cad1,
  0xced5db, 0xd8dee3, 0xe1e7eb, 0xe9eef2, 0xf0f4f7, 0xf6f9fb, 0xfbfdfe, 0xffffff,
};
// --- V ---------------------------------------------------------------------
static const uint32_t PAL_5[PAL_SIZE] = {
  0x05060c, 0x090b14, 0x0e111d, 0x131727, 0x191e32, 0x20263d, 0x272e49, 0x2f3755,
  0x384062, 0x414a6f, 0x4b557d, 0x57628b, 0x647099, 0x7280a8, 0x96a2c0, 0xf0c273,
};
// --- VI --------------------------------------------------------------------
static const uint32_t PAL_6[PAL_SIZE] = {
  0x2b2018, 0x3a2c20, 0x4a3929, 0x5a4733, 0x6b563e, 0x7c664a, 0x8d7657, 0x9e8765,
  0xaf9874, 0xbfa984, 0xcfba95, 0xddcaa7, 0xe9d9ba, 0xf2e6cd, 0xf9f0de, 0xfdf8ec,
};

#if !IMAGE_FIELDS
static const Atmos ATMOS[ATMOS_COUNT] = {
  { PAL_1, nullptr, 5200, 110,  90,  196, 7, 5, 190, "harbour before sunrise" },
  { PAL_2, nullptr, 6800, 190,  60,  262, 5, 8, 168, "pine woods, rain" },
  { PAL_3, nullptr, 4200,  90, 210,  330, 4, 4, 108, "a kitchen in the evening" },
  { PAL_4, nullptr, 9000,  40, 120,  392, 3, 2, 205, "snowfield, overcast" },
  { PAL_5, nullptr, 3400, 140,  70,  147, 9, 7,  96, "night train" },
  { PAL_6, nullptr, 7600,  70, 230,  220, 6, 3, 210, "road at noon, summer" },
};
#endif

#if IMAGE_FIELDS
// En la version de fotografias la identidad de cada campo la da su propia
// distribucion de color; el comportamiento temporal se hereda de la tabla de
// arriba, que sigue siendo autoria y no medida.
static const Atmos ATMOS[IMG_COUNT] = {
  { IMAGES[0].pal, IMAGES[0].rgb, 5200, 110,  90, 196, 7, 5, 190, IMAGES[0].source },
  { IMAGES[1].pal, IMAGES[1].rgb, 3400, 140,  70, 147, 9, 7,  96, IMAGES[1].source },
  { IMAGES[2].pal, IMAGES[2].rgb, 9000,  40, 120, 392, 3, 2, 205, IMAGES[2].source },
  { IMAGES[3].pal, IMAGES[3].rgb, 6800, 190,  60, 262, 5, 8, 168, IMAGES[3].source },
  { IMAGES[4].pal, IMAGES[4].rgb, 4200,  90, 210, 330, 4, 4, 108, IMAGES[4].source },
  { IMAGES[5].pal, IMAGES[5].rgb, 7600,  70, 230, 220, 6, 3, 210, IMAGES[5].source },
};
#endif

// numerales romanos: la unica etiqueta que lleva un campo en pantalla
static const char *const ATMOS_NUMERAL[8] = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII" };
