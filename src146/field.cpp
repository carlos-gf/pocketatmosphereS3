// Esta es la unica unidad que DEFINE las fotografias (ver images.h).
#define IMAGES_DEFINE
#include "field.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#if defined(ARDUINO) || defined(ESP32)
#include <Arduino.h>
#define FIELD_ALLOC(n) ps_malloc(n)
#else
#define FIELD_ALLOC(n) malloc(n)
#endif

// ---------------------------------------------------------------------------
// Ruido de valor 3D (x, y, tiempo) + fBm.
//
// La profundidad de reduccion controla CUATRO cosas a la vez, todas monotonas:
//
//   octavas          4 -> 1     menos estructura fina
//   frecuencia base  alta -> baja   manchas mas grandes
//   niveles de color 16 -> 3   menos colores distintos
//   recorrido        todo -> franja central   menos contraste
//
// Las cuatro empujan en la misma direccion, asi que "profundidad" es un eje
// ordenado de verdad y no una mezcla de efectos que se cancelan. La comprobacion
// esta en tools/depthcheck: entropia y numero de colores por profundidad.
// ---------------------------------------------------------------------------

static inline uint32_t hash3(int x, int y, int z) {
  uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 1442695041u;
  n = (n ^ (n >> 13)) * 1274126177u;
  return n ^ (n >> 16);
}

static inline float h01(int x, int y, int z) {
  return (float)(hash3(x, y, z) & 0xFFFFFF) * (1.0f / 16777215.0f);
}

static inline float smoothstep5(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

static float valueNoise(float x, float y, float z) {
  int xi = (int)floorf(x), yi = (int)floorf(y), zi = (int)floorf(z);
  float xf = x - xi, yf = y - yi, zf = z - zi;
  float u = smoothstep5(xf), v = smoothstep5(yf), w = smoothstep5(zf);

  float c000 = h01(xi, yi, zi),         c100 = h01(xi + 1, yi, zi);
  float c010 = h01(xi, yi + 1, zi),     c110 = h01(xi + 1, yi + 1, zi);
  float c001 = h01(xi, yi, zi + 1),     c101 = h01(xi + 1, yi, zi + 1);
  float c011 = h01(xi, yi + 1, zi + 1), c111 = h01(xi + 1, yi + 1, zi + 1);

  float x00 = c000 + (c100 - c000) * u, x10 = c010 + (c110 - c010) * u;
  float x01 = c001 + (c101 - c001) * u, x11 = c011 + (c111 - c011) * u;
  float y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
  return y0 + (y1 - y0) * w;
}

// Difuminado. Con pocos niveles, un Bayer puro se ve como una rejilla regular
// -y una rejilla es estructura de alta frecuencia, justo lo que la reduccion
// profunda debe eliminar-. Se mezcla Bayer 8x8 (que reparte bien) con un grano
// fijo por pixel (que rompe la periodicidad), y la amplitud total baja cuando
// quedan pocos niveles, para que el extremo reducido sea un lavado y no una trama.
static float BAYER8[64];
static float GRAIN[64 * 64];
static bool ditherReady = false;

static void initDither() {
  static const int B[64] = {
     0, 32,  8, 40,  2, 34, 10, 42,
    48, 16, 56, 24, 50, 18, 58, 26,
    12, 44,  4, 36, 14, 46,  6, 38,
    60, 28, 52, 20, 62, 30, 54, 22,
     3, 35, 11, 43,  1, 33,  9, 41,
    51, 19, 59, 27, 49, 17, 57, 25,
    15, 47,  7, 39, 13, 45,  5, 37,
    63, 31, 55, 23, 61, 29, 53, 21,
  };
  for (int i = 0; i < 64; i++) BAYER8[i] = (B[i] + 0.5f) / 64.0f - 0.5f;
  for (int i = 0; i < 64 * 64; i++) GRAIN[i] = h01(i & 63, i >> 6, 977) - 0.5f;
  ditherReady = true;
}

int fieldLevels(float depth) {
  int l = (int)(16.0f - 13.0f * depth + 0.5f);  // 16 -> 3
  return l < 3 ? 3 : (l > 16 ? 16 : l);
}

// Construye la tabla de colores para esta profundidad: `levels` entradas
// repartidas por la paleta, ya en RGB565.
static int buildLut(const FieldState &st, uint16_t *lut) {
  const Atmos &a = ATMOS[st.atmos];
  int levels = fieldLevels(st.depth);
  // Con la reduccion profunda la paleta tambien se estrecha. Si no, con 3 niveles
  // el ultimo cae en el acento (la farola, la ventana del tren) y reaparece un
  // detalle muy especifico justo donde tocaba haber menos: la reduccion tiene que
  // quitar informacion en todos los ejes a la vez, tambien en el color.
  float lo = 5.5f * st.depth;
  float hi = (PAL_SIZE - 1) - 6.0f * st.depth;
  for (int k = 0; k < levels; k++) {
    float f = (levels == 1) ? 0.5f : (float)k / (levels - 1);
    int pi = (int)(lo + f * (hi - lo) + 0.5f);
    if (pi < 0) pi = 0;
    if (pi > PAL_SIZE - 1) pi = PAL_SIZE - 1;
    lut[k] = C24(a.pal24[pi]);
  }
  return levels;
}

struct Span { float lo, hi; };

static Span depthSpan(float d) {
  Span s;
  s.lo = 0.00f + 0.32f * d;   // la reduccion profunda se queda en la franja central
  s.hi = 1.00f - 0.22f * d;
  return s;
}

// Rellena la rejilla gruesa con el campo escalar en 0..1.
static void computeGrid(float *g, int gw, int gh, const FieldState &st) {
  const Atmos &a = ATMOS[st.atmos];
  float d = st.depth;

  int octaves = (int)(4.0f - 3.0f * d + 0.5f);
  if (octaves < 1) octaves = 1;
  if (octaves > 4) octaves = 4;

  float freq = 2.60f - 1.70f * d;              // ciclos a lo ancho de la rejilla
  float grain = (a.grain / 255.0f) * (1.0f - d);  // el grano fino muere con la reduccion
  float contrast = 1.35f - 0.60f * d;

  // cada octava se desplaza a distinta velocidad: da sensacion de profundidad
  float t = st.t;

  for (int gy = 0; gy < gh; gy++) {
    for (int gx = 0; gx < gw; gx++) {
      float nx = (float)gx / (gw - 1);
      float ny = (float)gy / (gh - 1);
      float sum = 0.0f, amp = 1.0f, norm = 0.0f, f = freq;
      for (int o = 0; o < octaves; o++) {
        float oct = valueNoise(nx * f, ny * f * 1.16f, t * (0.7f + 0.45f * o) + o * 31.7f);
        float w = amp * (o == 0 ? 1.0f : (0.35f + 0.65f * grain));
        sum += oct * w;
        norm += w;
        amp *= 0.52f;
        f *= 2.03f;
      }
      float v = (norm > 0.0f) ? sum / norm : 0.5f;

      // curva en S: mas contraste con poca reduccion, mas plano con mucha
      v = 0.5f + (v - 0.5f) * contrast;
      v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
      v = smoothstep5(v) * 0.35f + v * 0.65f;

      // Sesgo direccional suave: la luz viene de arriba. Es una pista COMPARTIDA
      // -todo el mundo la lee igual- asi que empuja hacia la convergencia; se deja
      // pequena a proposito y por ambiente, no global.
      v += (a.tilt / 255.0f - 0.5f) * 0.34f * (0.5f - ny);

      // la hora del dia inclina el campo entero
      v += (st.daylight - 0.5f) * 0.14f;

      g[gy * gw + gx] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    }
  }
}

// ---------------------------------------------------------------------------
// Camino de FOTOGRAFIA. El escalar ya no sale de ruido sino de la luminancia de
// la imagen, y la profundidad de reduccion la desenfoca progresivamente.
//
// Aqui el eje hace por fin lo que la tesis dice: en 0 se ve el sitio, en 1 queda
// un lavado con los colores del sitio. Y al reducir aparece movimiento -el campo
// se ondula-, porque lo especifico esta quieto y lo evocativo respira.
// ---------------------------------------------------------------------------

static uint8_t *blurBufA = nullptr, *blurBufB = nullptr;

static void boxBlurH(const uint8_t *src, uint8_t *dst, int w, int h, int r) {
  if (r < 1) { memcpy(dst, src, (size_t)w * h); return; }
  int win = 2 * r + 1;
  for (int y = 0; y < h; y++) {
    const uint8_t *s = src + (size_t)y * w;
    uint8_t *d = dst + (size_t)y * w;
    int acc = s[0] * (r + 1);
    for (int i = 0; i < r && i < w; i++) acc += s[i];
    for (int x = 0; x < w; x++) {
      int add = (x + r < w) ? s[x + r] : s[w - 1];
      int sub = (x - r - 1 >= 0) ? s[x - r - 1] : s[0];
      acc += add - sub;
      d[x] = (uint8_t)(acc / win);
    }
  }
}

static void boxBlurV(const uint8_t *src, uint8_t *dst, int w, int h, int r) {
  if (r < 1) { memcpy(dst, src, (size_t)w * h); return; }
  int win = 2 * r + 1;
  for (int x = 0; x < w; x++) {
    int acc = src[x] * (r + 1);
    for (int i = 0; i < r && i < h; i++) acc += src[(size_t)i * w + x];
    for (int y = 0; y < h; y++) {
      int add = src[(size_t)((y + r < h) ? (y + r) : (h - 1)) * w + x];
      int sub = src[(size_t)((y - r - 1 >= 0) ? (y - r - 1) : 0) * w + x];
      acc += add - sub;
      dst[(size_t)y * w + x] = (uint8_t)(acc / win);
    }
  }
}

// Desenfoque a 16 bits. Con salida de 8 bits el campo tiene mesetas enormes y
// la ordenacion dentro de ellas cae en el orden de barrido: salen tiras rectas.
// Guardando el promedio con parte fraccionaria el campo vuelve a ser un gradiente.
static uint16_t *blur16A = nullptr, *blur16B = nullptr;

static void blurH8to16(const uint8_t *src, uint16_t *dst, int w, int h, int r) {
  int win = 2 * r + 1;
  for (int y = 0; y < h; y++) {
    const uint8_t *s = src + (size_t)y * w;
    uint16_t *d = dst + (size_t)y * w;
    int acc = s[0] * (r + 1);
    for (int i = 0; i < r && i < w; i++) acc += s[i];
    for (int x = 0; x < w; x++) {
      acc += ((x + r < w) ? s[x + r] : s[w - 1]) - ((x - r - 1 >= 0) ? s[x - r - 1] : s[0]);
      int v = acc * 255 / win;              // 0..65025
      d[x] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
    }
  }
}

static void blurV16(const uint16_t *src, uint16_t *dst, int w, int h, int r) {
  int win = 2 * r + 1;
  for (int x = 0; x < w; x++) {
    int64_t acc = (int64_t)src[x] * (r + 1);
    for (int i = 0; i < r && i < h; i++) acc += src[(size_t)i * w + x];
    for (int y = 0; y < h; y++) {
      acc += (int64_t)src[(size_t)((y + r < h) ? (y + r) : (h - 1)) * w + x]
           - (int64_t)src[(size_t)((y - r - 1 >= 0) ? (y - r - 1) : 0) * w + x];
      int64_t v = acc / win;
      dst[(size_t)y * w + x] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
    }
  }
}

static void blurH16(const uint16_t *src, uint16_t *dst, int w, int h, int r) {
  int win = 2 * r + 1;
  for (int y = 0; y < h; y++) {
    const uint16_t *s = src + (size_t)y * w;
    uint16_t *d = dst + (size_t)y * w;
    int64_t acc = (int64_t)s[0] * (r + 1);
    for (int i = 0; i < r && i < w; i++) acc += s[i];
    for (int x = 0; x < w; x++) {
      acc += (int64_t)((x + r < w) ? s[x + r] : s[w - 1])
           - (int64_t)((x - r - 1 >= 0) ? s[x - r - 1] : s[0]);
      d[x] = (uint16_t)(acc / win);
    }
  }
}

static const uint16_t *blurredLum16(const uint8_t *lum, int r) {
  if (!blur16A) {
    blur16A = (uint16_t *)FIELD_ALLOC((size_t)FIELD_W * FIELD_H * 2);
    blur16B = (uint16_t *)FIELD_ALLOC((size_t)FIELD_W * FIELD_H * 2);
    if (!blur16A || !blur16B) return nullptr;
  }
  if (r < 1) r = 1;
  // Cuatro cajas por eje. Con menos, el campo conserva detalle fino y la
  // ordenacion dentro de la celda lo persigue: sale grano en vez de mancha.
  blurH8to16(lum, blur16A, FIELD_W, FIELD_H, r);
  blurV16(blur16A, blur16B, FIELD_W, FIELD_H, r);
  const int radii[3] = { r, r * 2 / 3, r * 2 / 3 };
  for (int i = 0; i < 3; i++) {
    int rr = radii[i];
    if (rr < 1) continue;
    blurH16(blur16B, blur16A, FIELD_W, FIELD_H, rr);
    blurV16(blur16A, blur16B, FIELD_W, FIELD_H, rr);
  }
  return blur16B;
}

// radio de desenfoque por profundidad: nada al principio, casi todo al final
// El desenfoque NO toca los pixeles: solo forma el campo que decide el orden.
// Por eso puede llegar a radios enormes sin que el color se lave.
static int blurRadius(float d) {
  float r = 34.0f * d * d;
  return (int)(r + 0.5f);
}

static const uint8_t *blurredLum(const uint8_t *lum, float d) {
  int r = blurRadius(d);
  if (r < 1) return lum;
  if (!blurBufA) {
    blurBufA = (uint8_t *)FIELD_ALLOC((size_t)FIELD_W * FIELD_H);
    blurBufB = (uint8_t *)FIELD_ALLOC((size_t)FIELD_W * FIELD_H);
    if (!blurBufA || !blurBufB) return lum;
  }
  // dos pasadas de caja se parecen bastante a una gaussiana y cuestan O(n)
  boxBlurH(lum, blurBufA, FIELD_W, FIELD_H, r);
  boxBlurV(blurBufA, blurBufB, FIELD_W, FIELD_H, r);
  boxBlurH(blurBufB, blurBufA, FIELD_W, FIELD_H, r * 2 / 3);
  boxBlurV(blurBufA, blurBufB, FIELD_W, FIELD_H, r * 2 / 3);
  return blurBufB;
}

static uint8_t *fieldBuf = nullptr;
static uint32_t histCount[256], histStart[256], histRun[256];

// ---------------------------------------------------------------------------
// PERMUTACION HISTOGRAM PERFECT
//
// No se filtra la imagen: se REORDENA. El multiconjunto de pixeles es invariante
// -de ahi el nombre-, y lo unico que cambia con la profundidad es donde va cada
// uno. Por eso el extremo reducido no puede colapsar a un color plano: los
// pixeles oscuros y los claros siguen existiendo, solo dejan de estar ordenados.
//
// El campo de destino es la luminancia desenfocada (el desenfoque decide la
// ESCALA de la organizacion, no el color). Ordenar 67.200 pixeles por ese campo
// seria un sort; como el campo es de 8 bits, es una ordenacion por conteo: dos
// pasadas y ningun comparador. El desempate en orden de barrido es el mismo que
// el del sketch de Processing, y es lo que hace que las zonas planas salgan como
// barridos suaves en vez de ruido.
//
// Nota: una permutacion por rango es invariante a cualquier transformacion
// monotona del campo, asi que la hora del dia no toca este camino. No es un
// olvido: no hay forma de aplicarla sin dejar de ser histogram perfect.
// ---------------------------------------------------------------------------

#if IMAGE_FIELDS
// ---------------------------------------------------------------------------
// PERMUTACION HISTOGRAM PERFECT, LOCAL
//
// Antes se permutaba la imagen entera de una vez. Eso conserva el histograma
// global -que era el objetivo- pero pierde la APARIENCIA, porque el naranja de
// un santuario es el 1,29% de los pixeles: repartido por toda la pantalla de a
// un pixel, el ojo lo integra con el verde y desaparece. Lo que hace visible un
// acento no es su cantidad sino su CONCENTRACION.
//
// Asi que la permutacion ocurre dentro de celdas. Cada celda conserva su propio
// histograma exacto, luego la union sigue siendo histogram perfect; pero un
// pixel solo puede viajar hasta el borde de su celda. El dial de reduccion pasa
// a ser LA DISTANCIA QUE UN PIXEL PUEDE VIAJAR: de 1 px (identidad: la
// fotografia) hasta unos 60 px (un mosaico donde cada zona guarda su color).
//
// Las fronteras de celda siguen un campo de ruido en vez de una rejilla recta,
// para que se lean como manchas y no como bloques de JPEG.
// ---------------------------------------------------------------------------

static uint8_t *lumBuf = nullptr;      // luminancia 8 bits (entra al desenfoque)
static uint16_t *lum16 = nullptr;      // la misma, a 16 bits: ordena sin mesetas
static int lumFor = -1;                // que ambiente hay cacheado en lumBuf
static uint16_t *keyDst = nullptr;     // clave de destino, 16 bits
static uint16_t *cellBuf = nullptr;    // indice de celda por pixel
static uint32_t *ordSrc = nullptr, *ordDst = nullptr;  // posiciones ordenadas
static uint32_t *cellStart = nullptr, *cellRun = nullptr;
static uint32_t *tmpOrd = nullptr;      // orden de destino dentro de una celda
static uint32_t *radixTmp = nullptr;    // intermedio de la ordenacion por digitos
static uint16_t *scratch = nullptr;     // imagen permutada antes de volcarla

#define MAX_CELLS 24576

static const uint8_t *lumOf(int atmos) {
  if (lumFor == atmos && lumBuf) return lumBuf;
  if (!lumBuf) lumBuf = (uint8_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H);
  if (!lum16)  lum16  = (uint16_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 2);
  if (!lumBuf || !lum16) return nullptr;
  const uint16_t *p = ATMOS[atmos].rgb;
  for (size_t i = 0; i < (size_t)IMG_W * IMG_H; i++) {
    uint16_t c = p[i];
    // RGB565 -> luminancia. Se guarda a 16 bits porque a 8 hay tantos empates
    // que la ordenacion cae en el desempate de barrido y salen tiras rectas.
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    // Pesos para componentes 5/6/5 crudas -> 0..65535:
    //   0.2126*65535/31 = 449   0.7152*65535/63 = 744   0.0722*65535/31 = 153
    uint32_t y = 449u * r + 744u * g + 153u * b;
    if (y > 65535u) y = 65535u;
    // A 8 bits efectivos: los empates son frecuentes y los rompe el orden de
    // barrido, asi que pixeles de igual brillo viajan juntos y salen manchas.
    // A 16 bits reales cada pixel tiene su rango exacto y el resultado es grano.
    lum16[i] = (uint16_t)(y & 0xFF00u);
    lumBuf[i] = (uint8_t)(y >> 8);
  }
  lumFor = atmos;
  return lumBuf;
}

// Ordenacion estable por dos digitos de 8 bits. Dos pasadas, ningun comparador,
// y precision de 16 bits: es lo que evita las mesetas del campo desenfocado.
static void radix16(const uint32_t *in, uint32_t *out, uint32_t *tmp,
                    uint32_t cnt, const uint16_t *key) {
  uint32_t cnt0[256], cnt1[256], off[256];
  memset(cnt0, 0, sizeof cnt0);
  memset(cnt1, 0, sizeof cnt1);
  for (uint32_t k = 0; k < cnt; k++) {
    uint16_t v = key[in[k]];
    cnt0[v & 0xFF]++;
    cnt1[v >> 8]++;
  }
  uint32_t a = 0;
  for (int i = 0; i < 256; i++) { off[i] = a; a += cnt0[i]; }
  for (uint32_t k = 0; k < cnt; k++) tmp[off[key[in[k]] & 0xFF]++] = in[k];
  a = 0;
  for (int i = 0; i < 256; i++) { off[i] = a; a += cnt1[i]; }
  for (uint32_t k = 0; k < cnt; k++) out[off[key[tmp[k]] >> 8]++] = tmp[k];
}

// tamano de celda por profundidad: 1 px (identidad) -> 60 px
static int windowOf(float d) {
  int w = (int)(1.0f + d * d * 59.0f + 0.5f);
  return w < 1 ? 1 : w;
}

static bool ensureBuffers() {
  if (!keyDst)    keyDst    = (uint16_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 2);
  if (!cellBuf)   cellBuf   = (uint16_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 2);
  if (!ordSrc)    ordSrc    = (uint32_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 4);
  if (!ordDst)    ordDst    = (uint32_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 4);
  if (!cellStart) cellStart = (uint32_t *)FIELD_ALLOC(MAX_CELLS * 4);
  if (!cellRun)   cellRun   = (uint32_t *)FIELD_ALLOC(MAX_CELLS * 4);
  if (!tmpOrd)    tmpOrd    = (uint32_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 4);
  if (!scratch)   scratch   = (uint16_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 2);
  if (!radixTmp)  radixTmp  = (uint32_t *)FIELD_ALLOC((size_t)IMG_W * IMG_H * 4);
  return keyDst && cellBuf && ordSrc && ordDst && cellStart && cellRun && tmpOrd && scratch && radixTmp;
}

static void renderImage(uint16_t *out, int outStride, int outRows, int x0, int y0,
                        int w, int h, const FieldState &st) {
  const uint16_t *img = ATMOS[st.atmos].rgb;
  const uint8_t *lum = lumOf(st.atmos);
  (void)lum;
  if (!lum || !ensureBuffers()) return;

  const int W = IMG_W, H = IMG_H;
  const size_t n = (size_t)W * H;
  int win = windowOf(st.depth);

  // profundidad 0: la fotografia, intacta. Sin coste y sin dudas.
  if (win <= 1) {
    for (int y = 0; y < h; y++) {
      uint16_t *dst = out + (y0 + y) * outStride + x0;
      int sy = (int)((int64_t)y * H / (h > 1 ? h : 1));
      for (int x = 0; x < w; x++) {
        int sx = (int)((int64_t)x * W / (w > 1 ? w : 1));
        dst[x] = FIELD_OUT(img[(size_t)sy * W + sx]);
      }
    }
    return;
  }

  // --- campo de destino: luminancia desenfocada, ondulando con el tiempo -----
  int blurR = win * 3 / 5;
  const uint16_t *bl = blurredLum16(lumBuf, blurR);
  if (!bl) return;
  float amp = 0.10f * win * st.agitation;   // solo el IMU ondula el campo
  float tt = st.t * 0.5f;
  float fq = 1.0f / (win * 2.2f + 8.0f);
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      float fx = x, fy = y;
      if (amp > 0.6f) {
        fx += (valueNoise(x * fq, y * fq, tt) - 0.5f) * amp;
        fy += (valueNoise(x * fq + 31.0f, y * fq + 17.0f, tt + 5.0f) - 0.5f) * amp;
      }
      int ix = (int)fx, iy = (int)fy;
      ix = ix < 0 ? 0 : (ix > W - 1 ? W - 1 : ix);
      iy = iy < 0 ? 0 : (iy > H - 1 ? H - 1 : iy);
      keyDst[(size_t)y * W + x] = bl[(size_t)iy * W + ix];
    }
  }

  // --- celdas organicas: la rejilla se deforma con un ruido de baja frecuencia
  float cf = 1.0f / (win * 1.6f > 6.0f ? win * 1.6f : 6.0f);
  float ct = 3.0f + st.t * (0.25f + 0.6f * st.agitation);
  int cw = (W + win - 1) / win;
  int ch = (H + win - 1) / win;
  // Si la rejilla no cabe en la tabla de celdas, se agranda la celda hasta que
  // quepa. Sin esto, el indice se saturaba y celdas de toda la imagen caian en
  // el mismo cubo: salian tiras horizontales en vez de manchas.
  while (cw * ch > MAX_CELLS) { win++; cw = (W + win - 1) / win; ch = (H + win - 1) / win; }
  int nc = cw * ch;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      // El movimiento vive AQUI: la retícula de celdas deriva lentamente, asi que
      // las manchas se reorganizan enteras en vez de hervir pixel a pixel.
      float wx = x + (valueNoise(x * cf, y * cf, ct) - 0.5f) * win * 0.9f;
      float wy = y + (valueNoise(x * cf + 13.0f, y * cf + 7.0f, ct + 9.0f) - 0.5f) * win * 0.9f;
      int cx = (int)(wx < 0.0f ? 0.0f : wx) / win;
      int cy = (int)(wy < 0.0f ? 0.0f : wy) / win;
      if (cx < 0) cx = 0; else if (cx > cw - 1) cx = cw - 1;
      if (cy < 0) cy = 0; else if (cy > ch - 1) cy = ch - 1;
      cellBuf[(size_t)y * W + x] = (uint16_t)(cy * cw + cx);
    }
  }

  // --- ordenacion por conteo en dos niveles: primero celda, luego valor ------
  // Sin comparadores: dos pasadas por celda y una tabla de 256 por celda.
  memset(cellStart, 0, (size_t)nc * 4);
  for (size_t i = 0; i < n; i++) cellStart[cellBuf[i]]++;
  uint32_t acc = 0;
  for (int c = 0; c < nc; c++) { uint32_t k = cellStart[c]; cellStart[c] = acc; acc += k; cellRun[c] = 0; }

  // posiciones agrupadas por celda (orden de barrido dentro de cada una)
  for (size_t i = 0; i < n; i++) {
    uint16_t c = cellBuf[i];
    ordSrc[cellStart[c] + cellRun[c]++] = (uint32_t)i;
  }

  // Dentro de cada celda, dos ordenaciones y un reparto:
  //   origen  ordenado por la luminancia de la foto
  //   destino ordenado por el campo
  //   el k-esimo pixel mas oscuro va al k-esimo sitio mas oscuro
  // El multiconjunto de la celda no cambia: histogram perfect por construccion.
  for (int c = 0; c < nc; c++) {
    uint32_t beg = cellStart[c], cnt = cellRun[c];
    if (!cnt) continue;
    radix16(ordSrc + beg, ordDst + beg, radixTmp + beg, cnt, lum16);
    radix16(ordSrc + beg, tmpOrd + beg, radixTmp + beg, cnt, keyDst);
    for (uint32_t k = 0; k < cnt; k++) scratch[tmpOrd[beg + k]] = img[ordDst[beg + k]];
  }

  // volcado al framebuffer (con escalado si es un recuadro pequeno)
  for (int y = 0; y < h; y++) {
    uint16_t *dst = out + (y0 + y) * outStride + x0;
    int sy = (int)((int64_t)y * H / (h > 1 ? h : 1));
    for (int x = 0; x < w; x++) {
      int sx = (int)((int64_t)x * W / (w > 1 ? w : 1));
      dst[x] = FIELD_OUT(scratch[(size_t)sy * W + sx]);
    }
  }
}

#endif  // IMAGE_FIELDS

// La pantalla es un disco. Lo que cae fuera del circulo no se ve, pero si se
// deja escrito se cuela por los bordes del cristal como una linea clara.
void fieldMaskCircle(uint16_t *fb, uint16_t bg) {
  const int cx = FIELD_W / 2, cy = FIELD_H / 2, r = FIELD_W / 2;
  const int r2 = r * r;
  for (int y = 0; y < FIELD_H; y++) {
    int dy = y - cy, dy2 = dy * dy;
    uint16_t *row = fb + (size_t)y * FIELD_W;
    for (int x = 0; x < FIELD_W; x++) {
      int dx = x - cx;
      if (dx * dx + dy2 > r2) row[x] = bg;
    }
  }
}

static void renderInto(uint16_t *out, int outStride, int outRows, int x0, int y0, int w, int h,
                       const FieldState &st) {
  // Recorte: un recuadro que se salga del framebuffer no debe escribir fuera.
  // En el aparato eso seria un reinicio en bucle, no un pixel raro.
  if (x0 < 0) { w += x0; x0 = 0; }
  if (y0 < 0) { h += y0; y0 = 0; }
  if (x0 + w > outStride) w = outStride - x0;
  if (y0 + h > outRows) h = outRows - y0;
  if (w <= 0 || h <= 0) return;

#if IMAGE_FIELDS
  if (ATMOS[st.atmos].rgb) {
    renderImage(out, outStride, outRows, x0, y0, w, h, st);
    return;
  }
#endif

  uint16_t lut[16];
  int levels = buildLut(st, lut);
  Span sp = depthSpan(st.depth);

  if (!ditherReady) initDither();

  int gw = FIELD_GW, gh = FIELD_GH;
  static float grid[FIELD_GW * FIELD_GH];
  computeGrid(grid, gw, gh, st);

  float sx = (float)(gw - 1) / (float)(w > 1 ? w - 1 : 1);
  float sy = (float)(gh - 1) / (float)(h > 1 ? h - 1 : 1);
  float top = (float)(levels - 1);
  // con 16 niveles el difuminado es invisible y util; con 3 estorba
  // el difuminado tambien es estructura: casi apagado cuando quedan pocos niveles
  float damp = 0.10f + 0.90f * ((float)(levels - 3) / 13.0f);

  for (int y = 0; y < h; y++) {
    float gyf = y * sy;
    int gy = (int)gyf;
    if (gy > gh - 2) gy = gh - 2;
    float fy = gyf - gy;
    const float *r0 = grid + gy * gw;
    const float *r1 = r0 + gw;
    uint16_t *dst = out + (y0 + y) * outStride + x0;
    const float *bay = BAYER8 + ((y & 7) << 3);
    const float *grn = GRAIN + ((y & 63) << 6);

    for (int x = 0; x < w; x++) {
      float gxf = x * sx;
      int gx = (int)gxf;
      if (gx > gw - 2) gx = gw - 2;
      float fx = gxf - gx;

      float a0 = r0[gx] + (r0[gx + 1] - r0[gx]) * fx;
      float a1 = r1[gx] + (r1[gx + 1] - r1[gx]) * fx;
      float v = a0 + (a1 - a0) * fy;

      v = sp.lo + v * (sp.hi - sp.lo);

      float dth = (bay[x & 7] * 0.62f + grn[x & 63] * 0.38f) * damp;
      int k = (int)(v * top + dth + 0.5f);
      if (k < 0) k = 0;
      if (k > levels - 1) k = levels - 1;
      dst[x] = FIELD_OUT(lut[k]);
    }
  }
}

void fieldRender(uint16_t *fb, const FieldState &st) {
  renderInto(fb, FIELD_W, FIELD_H, 0, 0, FIELD_W, FIELD_H, st);
}

void fieldSwatch(uint16_t *fb, int fbw, int fbh, int x0, int y0, int w, int h, const FieldState &st) {
  renderInto(fb, fbw, fbh, x0, y0, w, h, st);
}

uint16_t fieldMidColor(const FieldState &st) {
  uint16_t lut[16];
  int levels = buildLut(st, lut);
  return lut[levels / 2];
}

bool fieldIsLight(const FieldState &st) {
  uint16_t c = fieldMidColor(st);
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  int lum = (r * 255 / 31) * 30 + (g * 255 / 63) * 59 + (b * 255 / 31) * 11;
  return lum > 12500;  // 0..25500
}


#if defined(FIELD_HOST) && IMAGE_FIELDS
// Solo para el banco de pruebas del host: deja ver el campo y las celdas.
#include <cstdio>
void dbgDump(const FieldState &st, const char *fieldPath, const char *cellPath) {
  static uint16_t fb[FIELD_W * FIELD_H];
  fieldRender(fb, st);
  FILE *f = fopen(fieldPath, "wb");
  fprintf(f, "P5\n%d %d\n255\n", FIELD_W, FIELD_H);
  for (size_t i = 0; i < (size_t)FIELD_W * FIELD_H; i++) { unsigned char v = keyDst[i] >> 8; fwrite(&v,1,1,f); }
  fclose(f);
  f = fopen(cellPath, "wb");
  fprintf(f, "P5\n%d %d\n255\n", FIELD_W, FIELD_H);
  for (size_t i = 0; i < (size_t)FIELD_W * FIELD_H; i++) { unsigned char v = (unsigned char)(cellBuf[i] * 37); fwrite(&v,1,1,f); }
  fclose(f);
}
#endif
