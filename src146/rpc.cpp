#include "rpc.h"
#include <string.h>
#include <math.h>

#ifdef RPC_HOST
  #include <stdlib.h>
  #include <stdio.h>
  #define RPC_ALLOC(n) malloc(n)
  #define RPC_FREE(p)  free(p)
  extern const uint16_t *hostImage(int atmos);   // 412x412 RGB565
  #define RPC_SRC(a) hostImage(a)
  #define RPC_OUT(c) (c)
  #define RPC_MS()   (0u)
#else
  #include <Arduino.h>
  #include "atmos.h"
  #include "field.h"          // FIELD_OUT: sin swap en Arduino_GFX
  #define RPC_ALLOC(n) ps_malloc(n)
  #define RPC_FREE(p)  free(p)
  #define RPC_SRC(a) (ATMOS[a].rgb)
  #define RPC_OUT(c) FIELD_OUT(c)
  #define RPC_MS()   millis()
#endif

#define SRC_W 412
#define SRC_H 412

// ---------------------------------------------------------------------------
// Mapa de profundidad. Igual que el sketch.
// ---------------------------------------------------------------------------
#define Q_LOG_MAX 6.0f
#define Q_LOG_MIN 1.0f
#define SIGMA_MIN 3.5f
#define SIGMA_MAX 35.0f
#define SIGMA_GAMMA 1.3f
#define HUE_W_SAT 0.08f
#define HUE_W_VAL 0.02f

int rpcQForDepth(float d) {
  if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
  float e = Q_LOG_MAX - (Q_LOG_MAX - Q_LOG_MIN) * d;
  int q = (int)(powf(2.0f, e) + 0.5f);
  return q < 1 ? 1 : q;
}

float rpcSigmaForDepth(float d, int w) {
  if (d < 0.0f) d = 0.0f; else if (d > 1.0f) d = 1.0f;
  float s = SIGMA_MIN + (SIGMA_MAX - SIGMA_MIN) * powf(d, SIGMA_GAMMA);
  return s * ((float)w / 900.0f);
}

// Anchuras de las tres cajas cuya convolucion mejor imita una gaussiana de este
// sigma. Son ENTERAS, y de ahi viene el escalonado de la respiracion.
static void boxesForGauss(float sigma, int *b) {
  float wIdeal = sqrtf((12.0f * sigma * sigma / 3.0f) + 1.0f);
  int wl = (int)floorf(wIdeal);
  if ((wl & 1) == 0) wl--;
  if (wl < 1) wl = 1;
  int wu = wl + 2;
  float mIdeal = (12.0f * sigma * sigma - 3.0f * wl * wl - 12.0f * wl - 9.0f) /
                 (-4.0f * wl - 4.0f);
  int m = (int)(mIdeal + 0.5f);
  for (int i = 0; i < 3; i++) b[i] = (i < m) ? wl : wu;
}

// ---------------------------------------------------------------------------
// Ruido de TEXTURE. El hash entero del sketch, no un random(): un estimulo
// tiene que regenerarse identico.
// ---------------------------------------------------------------------------
static inline float hashNoise(uint32_t i) {
  uint64_t x = (uint64_t)i * 0x9E3779B97F4A7C15ULL +
               (uint64_t)RPC_NOISE_SEED * 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27; x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return (float)((x >> 11) & 0xFFFFFFULL) * (1.0f / 16777216.0f);
}

// atan2 en VUELTAS (0..1), aproximado. Solo tiene que ORDENAR posiciones, y lo
// que ordena ya son datos desenfocados. Medido contra atan2f: error maximo
// 0.086 grados y monotona en toda la vuelta.
static inline float atan2turns(float y, float x) {
  float ax = fabsf(x), ay = fabsf(y);
  if (ax == 0.0f && ay == 0.0f) return 0.0f;
  float r, t;
  if (ax >= ay) {
    r = ay / ax;
    t = 0.125f * r - r * (r - 1.0f) * (0.03895f + 0.01055f * r);
  } else {
    r = ax / ay;
    t = 0.25f - (0.125f * r - r * (r - 1.0f) * (0.03895f + 0.01055f * r));
  }
  if (x < 0.0f) t = 0.5f - t;
  if (y < 0.0f) t = 1.0f - t;
  return t;
}

// ---------------------------------------------------------------------------
// NIVELES
//
// Dos resoluciones de trabajo. La fina (206) es lo que se mira; la gruesa (103)
// es lo que se ve MIENTRAS SE ARRASTRA. Cuesta la cuarta parte y a simple vista
// apenas se distingue -el campo ya esta desenfocado-, asi que el pulgar mueve
// algo que responde en vez de esperar a una reduccion entera por fotograma.
//
// Lo que hace lento el bucle no es la operacion: es RECALCULARLA cuando no ha
// cambiado nada. Q y las anchuras de las cajas son enteras, asi que arrastrar
// el dedo medio milimetro casi siempre da EL MISMO estado y el mismo resultado
// bit a bit. Todo lo que sigue esta construido alrededor de eso: se cachea por
// estado y no se recalcula jamas un estado ya visto.
// ---------------------------------------------------------------------------
struct Level {
  int w, h, n;
  uint16_t *img = nullptr;    // RGB565 reducida
  uint16_t *lum = nullptr;    // luminancia 0..32767 (cabe en int16 para el desenfoque)
  uint16_t *hueT = nullptr;   // termino de tono, 0..1023
  int16_t  *hcos = nullptr;   // vector de tono pesado por croma, +-4096
  int16_t  *hsin = nullptr;
  int atmos = -1;
};

static Level LF = { RPC_W, RPC_H, RPC_W * RPC_H };
static Level LC = { RPC_CW, RPC_CH, RPC_CW * RPC_CH };

// Buffers de trabajo, dimensionados para el nivel fino y compartidos por ambos.
// A 16 bits, no a 32: el desenfoque es el coste dominante y son todo lecturas y
// escrituras a PSRAM, asi que la mitad de bytes es casi la mitad de tiempo.
static int16_t  *bufA = nullptr, *bufB = nullptr, *fcos = nullptr;
static uint16_t *keyD = nullptr, *keyH = nullptr, *keyS = nullptr;
static uint16_t *ordS = nullptr, *ordD = nullptr, *ordT = nullptr, *ordX = nullptr;

static bool ensureWork() {
  const size_t N = (size_t)RPC_W * RPC_H;
  if (!bufA) bufA = (int16_t  *)RPC_ALLOC(N * 2);
  if (!bufB) bufB = (int16_t  *)RPC_ALLOC(N * 2);
  if (!fcos) fcos = (int16_t  *)RPC_ALLOC(N * 2);
  if (!keyD) keyD = (uint16_t *)RPC_ALLOC(N * 2);
  if (!keyH) keyH = (uint16_t *)RPC_ALLOC(N * 2);
  if (!keyS) keyS = (uint16_t *)RPC_ALLOC(N * 2);
  if (!ordS) ordS = (uint16_t *)RPC_ALLOC(N * 2);
  if (!ordD) ordD = (uint16_t *)RPC_ALLOC(N * 2);
  if (!ordT) ordT = (uint16_t *)RPC_ALLOC(N * 2);
  if (!ordX) ordX = (uint16_t *)RPC_ALLOC(N * 2);
  return bufA && bufB && fcos && keyD && keyH && keyS && ordS && ordD && ordT && ordX;
}

// ---------------------------------------------------------------------------
// Preparacion por nivel y ambiente. Reduce 412 -> w promediando bloques, y de
// ahi saca luminancia, termino de tono y el vector de tono. NO depende de la
// profundidad: se hace una vez por fotografia y por nivel.
// ---------------------------------------------------------------------------
static bool prepare(Level &L, int atmos) {
  if (L.atmos == atmos && L.img) return true;
  const size_t n = (size_t)L.n;
  if (!L.img)  L.img  = (uint16_t *)RPC_ALLOC(n * 2);
  if (!L.lum)  L.lum  = (uint16_t *)RPC_ALLOC(n * 2);
  if (!L.hueT) L.hueT = (uint16_t *)RPC_ALLOC(n * 2);
  if (!L.hcos) L.hcos = (int16_t  *)RPC_ALLOC(n * 2);
  if (!L.hsin) L.hsin = (int16_t  *)RPC_ALLOC(n * 2);
  if (!L.img || !L.lum || !L.hueT || !L.hcos || !L.hsin) return false;

  const int sc = SRC_W / L.w;                 // 2 para 206, 4 para 103
  const uint16_t *s = RPC_SRC(atmos);
  for (int y = 0; y < L.h; y++) {
    for (int x = 0; x < L.w; x++) {
      uint32_t rr = 0, gg = 0, bb = 0;
      int sx = x * sc, sy = y * sc;
      for (int dy = 0; dy < sc; dy++)
        for (int dx = 0; dx < sc; dx++) {
          uint16_t c = s[(size_t)(sy + dy) * SRC_W + (sx + dx)];
          rr += (c >> 11) & 0x1F; gg += (c >> 5) & 0x3F; bb += c & 0x1F;
        }
      const uint32_t m = (uint32_t)sc * sc;
      uint32_t r = rr / m, g = gg / m, b = bb / m;
      int i = y * L.w + x;
      L.img[i] = (uint16_t)((r << 11) | (g << 5) | b);

      // Luminancia sobre las componentes 5/6/5 CRUDAS. Los pesos son
      // 0.2126*65535/31 = 449, 0.7152*65535/63 = 744, 0.0722*65535/31 = 153,
      // y se guarda a la MITAD para que quepa en int16 con signo. Los pesos de
      // 0..255 aplicados aqui fue lo que desbordo y saturo el campo aquella vez;
      // se encontro midiendo el campo contra el prototipo, no mirandolo.
      uint32_t Y = 449u * r + 744u * g + 153u * b;
      if (Y > 65535u) Y = 65535u;
      L.lum[i] = (uint16_t)(Y >> 1);           // 0..32767

      float fr = r * (1.0f / 31.0f), fg = g * (1.0f / 63.0f), fb = b * (1.0f / 31.0f);
      float mx = fr > fg ? (fr > fb ? fr : fb) : (fg > fb ? fg : fb);
      float mn = fr < fg ? (fr < fb ? fr : fb) : (fg < fb ? fg : fb);
      float dl = mx - mn;
      float hh;
      if (dl == 0.0f)      hh = 0.0f;
      else if (mx == fr)   hh = ((fg - fb) / dl) * (1.0f / 6.0f);
      else if (mx == fg)   hh = ((fb - fr) / dl + 2.0f) * (1.0f / 6.0f);
      else                 hh = ((fr - fg) / dl + 4.0f) * (1.0f / 6.0f);
      if (hh < 0.0f) hh += 1.0f;
      float ss = (mx == 0.0f) ? 0.0f : dl / mx;

      float t = (hh + ss * HUE_W_SAT + mx * HUE_W_VAL) /
                (1.0f + HUE_W_SAT + HUE_W_VAL);
      int ht = (int)(t * 1023.0f + 0.5f);
      L.hueT[i] = (uint16_t)(ht < 0 ? 0 : (ht > 1023 ? 1023 : ht));

      // El tono es un ANGULO: se promedia como vector, o la media de dos rojos
      // profundos sale cian. Y pesado por croma, para que los grises -que no
      // tienen tono que aportar- no arrastren el resultado.
      float wgt = ss * mx, ang = 6.28318531f * hh;
      L.hcos[i] = (int16_t)(cosf(ang) * wgt * 4096.0f);
      L.hsin[i] = (int16_t)(sinf(ang) * wgt * 4096.0f);
    }
  }
  L.atmos = atmos;
  return true;
}

// ---------------------------------------------------------------------------
// Desenfoque de caja, sumas corridas: O(1) por pixel sea cual sea sigma.
//
// La pasada VERTICAL se hace por bloques de columnas, no columna a columna. En
// PSRAM recorrer una columna es ir saltando de 412 en 412 bytes y cada lectura
// se trae una rafaga entera para usar dos bytes. Con 16 columnas a la vez, cada
// rafaga se aprovecha completa y la pasada pasa a leer casi en secuencia.
// ---------------------------------------------------------------------------
#define VBLK 16

static void boxH(const int16_t *a, int16_t *d, int w, int h, int r) {
  if (r > (w - 1) / 2) r = (w - 1) / 2;
  if (r < 1) { memcpy(d, a, (size_t)w * h * 2); return; }
  const int32_t div = 2 * r + 1, half = div / 2;
  for (int y = 0; y < h; y++) {
    const int16_t *row = a + (size_t)y * w;
    int16_t *dst = d + (size_t)y * w;
    int32_t acc = (int32_t)row[0] * (r + 1);
    for (int i = 1; i <= r; i++) acc += row[i];
    for (int x = 0; x < w; x++) {
      dst[x] = (int16_t)((acc + half) / div);
      int ia = x - r, ib = x + r + 1;
      acc += row[ib < w ? ib : w - 1];
      acc -= row[ia > 0 ? ia : 0];
    }
  }
}

static void boxV(const int16_t *a, int16_t *d, int w, int h, int r) {
  if (r > (h - 1) / 2) r = (h - 1) / 2;
  if (r < 1) { memcpy(d, a, (size_t)w * h * 2); return; }
  const int32_t div = 2 * r + 1, half = div / 2;
  int32_t acc[VBLK];
  for (int x0 = 0; x0 < w; x0 += VBLK) {
    int nb = (w - x0) < VBLK ? (w - x0) : VBLK;
    for (int k = 0; k < nb; k++) acc[k] = (int32_t)a[x0 + k] * (r + 1);
    for (int i = 1; i <= r; i++) {
      const int16_t *row = a + (size_t)i * w + x0;
      for (int k = 0; k < nb; k++) acc[k] += row[k];
    }
    for (int y = 0; y < h; y++) {
      int16_t *dst = d + (size_t)y * w + x0;
      for (int k = 0; k < nb; k++) dst[k] = (int16_t)((acc[k] + half) / div);
      int ia = y - r, ib = y + r + 1;
      const int16_t *add = a + (size_t)(ib < h ? ib : h - 1) * w + x0;
      const int16_t *sub = a + (size_t)(ia > 0 ? ia : 0) * w + x0;
      for (int k = 0; k < nb; k++) acc[k] += add[k] - sub[k];
    }
  }
}

static void blur3(int16_t *buf, int16_t *scratch, int w, int h, const int *boxes) {
  for (int i = 0; i < 3; i++) {
    int r = (boxes[i] - 1) / 2;
    if (r < 1) continue;
    boxH(buf, scratch, w, h, r);
    boxV(scratch, buf, w, h, r);
  }
}

// ---------------------------------------------------------------------------
// Radix estable de dos digitos de 8 bits sobre indices de 16 bits.
// OJO con los alias: tmp NO puede ser in. (out == in si vale.)
// ---------------------------------------------------------------------------
static void radix16(const uint16_t *in, uint16_t *out, uint16_t *tmp,
                    int32_t cnt, const uint16_t *key) {
  uint32_t c0[256], c1[256], off[256];
  memset(c0, 0, sizeof c0);
  memset(c1, 0, sizeof c1);
  for (int32_t k = 0; k < cnt; k++) {
    uint16_t v = key[in[k]];
    c0[v & 0xFF]++; c1[v >> 8]++;
  }
  uint32_t a = 0;
  for (int i = 0; i < 256; i++) { off[i] = a; a += c0[i]; }
  for (int32_t k = 0; k < cnt; k++) tmp[off[key[in[k]] & 0xFF]++] = in[k];
  a = 0;
  for (int i = 0; i < 256; i++) { off[i] = a; a += c1[i]; }
  for (int32_t k = 0; k < cnt; k++) out[off[key[tmp[k]] >> 8]++] = tmp[k];
}

// ---------------------------------------------------------------------------
// Cronometraje. "Va lento" no es un diagnostico; el comando TIME por USB da los
// milisegundos de cada etapa medidos EN LA PLACA, que es el unico sitio donde
// el numero significa algo.
// ---------------------------------------------------------------------------
static RpcTiming gT;
const RpcTiming &rpcTiming() { return gT; }

// ---------------------------------------------------------------------------
// UNA reduccion, en el nivel dado, con Q y sigma explicitos.
// ---------------------------------------------------------------------------
static bool reduceQS(Level &L, int atmos, int Q, float sigma, uint16_t *dst) {
  if (!prepare(L, atmos) || !ensureWork()) return false;
  const int32_t n = L.n;
  const int w = L.w, h = L.h;
  int boxes[3];
  boxesForGauss(sigma, boxes);
  uint32_t t0 = RPC_MS();

  // --- clave de destino: luminancia desenfocada ------------------------------
  memcpy(bufA, L.lum, (size_t)n * 2);
  blur3(bufA, bufB, w, h, boxes);
  for (int32_t i = 0; i < n; i++) {
    int v = bufA[i];
    keyD[i] = (uint16_t)(v < 0 ? 0 : (v > 32767 ? 32767 : v));
  }

  // --- campo de tono: los dos vectores desenfocados, y su angulo -------------
  memcpy(bufA, L.hcos, (size_t)n * 2);
  blur3(bufA, bufB, w, h, boxes);
  memcpy(fcos, bufA, (size_t)n * 2);
  memcpy(bufA, L.hsin, (size_t)n * 2);
  blur3(bufA, bufB, w, h, boxes);
  uint32_t t1 = RPC_MS();

  for (int32_t i = 0; i < n; i++) {
    float t = atan2turns((float)bufA[i], (float)fcos[i]);
    int v = (int)(t * 65535.0f + 0.5f);
    keyH[i] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
  }

  // --- clave de origen: banda de luminancia + tono dentro de la banda --------
  // Se clampea la BANDA, no el cociente: por el cociente, un blanco puro se
  // colaba como banda == Q y se salia de la tabla.
  int32_t counts[64];
  int nb = Q > 64 ? 64 : Q;
  memset(counts, 0, sizeof(int32_t) * nb);
  const float invLum = 1.0f / 32767.0f;
  for (int32_t i = 0; i < n; i++) {
    float ln = L.lum[i] * invLum + RPC_TEXTURE * (hashNoise((uint32_t)i) - 0.5f);
    int bi = (int)floorf(ln * (float)Q);
    if (bi < 0) bi = 0; else if (bi > nb - 1) bi = nb - 1;
    counts[bi]++;
    // banda en los 6 bits altos, tono en los 10 bajos: el tono nunca alcanza 1,
    // asi que las bandas no se mezclan.
    keyS[i] = (uint16_t)(((uint16_t)bi << 10) | (L.hueT[i] & 0x3FF));
  }
  uint32_t t2 = RPC_MS();

  for (int32_t i = 0; i < n; i++) ordS[i] = (uint16_t)i;
  radix16(ordS, ordT, ordX, n, keyS);   // ordT = origen ordenado por banda+tono
  radix16(ordS, ordD, ordX, n, keyD);   // ordD = destino ordenado por el campo

  // --- colocacion del color dentro de cada banda -----------------------------
  // Sin esto, el tono se reparte a lo largo del gradiente fino de luminancia y
  // cada color sale en hilos finos esparcidos por la imagen: a distancia de
  // lectura se promedian con el fondo y el color DESAPARECE. Cada banda se
  // queda con la misma region de destino -la rebanada esta definida por RANGO,
  // asi que la escalera de profundidad no se toca- pero las posiciones de
  // dentro se reordenan por el campo de tono.
  int32_t off = 0;
  for (int k = 0; k < nb; k++) {
    int32_t c = counts[k];
    if (c > 1) radix16(ordD + off, ordD + off, ordX, c, keyH);
    off += c;
  }
  uint32_t t3 = RPC_MS();

  const uint16_t *im = L.img;
  for (int32_t k = 0; k < n; k++) dst[ordD[k]] = im[ordT[k]];
  uint32_t t4 = RPC_MS();

  gT.w = w;
  gT.blur = t1 - t0; gT.keys = t2 - t1; gT.sort = t3 - t2;
  gT.scatter = t4 - t3; gT.total = t4 - t0;
  return true;
}

// ---------------------------------------------------------------------------
// Volcado al panel. Vecino mas proximo, factor entero. Medido: es lo que
// devuelve la energia de escala fina de la referencia (5.99 frente a 5.89);
// con bilineal cae a 1.92, porque la interpolacion se come el grano.
// ---------------------------------------------------------------------------
static void blitN(const uint16_t *s, int w, int h, uint16_t *out412) {
  const int sc = SRC_W / w;
  for (int y = 0; y < h; y++) {
    const uint16_t *sr = s + (size_t)y * w;
    for (int dy = 0; dy < sc; dy++) {
      uint16_t *d = out412 + (size_t)(y * sc + dy) * SRC_W;
      for (int x = 0; x < w; x++) {
        uint16_t c = RPC_OUT(sr[x]);
        uint16_t *p = d + x * sc;
        for (int dx = 0; dx < sc; dx++) p[dx] = c;
      }
    }
  }
}

// Volcado SUAVE, solo para el nivel grueso mientras el dedo se mueve.
//
// Con vecino mas proximo, el nivel grueso a poca reduccion se ve a cuadros: la
// energia de bloque medida a d=0.08 es 15.1 frente a 7.9 del nivel fino, casi
// el doble. Y se nota justo ahi, en la parte CLARA de la escalera, porque es
// donde queda detalle que perder; a d=0.35 ya da 6.8 y no se aprecia.
//
// El vecino es el volcado correcto para lo que se MIRA -devuelve el grano de la
// referencia, 5.99 frente a 5.89, mientras que el bilineal lo hunde a 1.92-,
// pero para un adelanto de medio segundo mientras se arrastra, el grano no
// importa y los cuadros si. Asi que: suave mientras se mueve, exacto al parar.
static void blitSmooth(const uint16_t *s, int w, int h, uint16_t *out412) {
  const int sc = SRC_W / w;
  for (int oy = 0; oy < SRC_H; oy++) {
    int sy = oy / sc, fy = oy % sc;
    int sy1 = (sy + 1 < h) ? sy + 1 : sy;
    int wy = (fy * 256) / sc, iwy = 256 - wy;
    const uint16_t *r0 = s + (size_t)sy * w, *r1 = s + (size_t)sy1 * w;
    uint16_t *d = out412 + (size_t)oy * SRC_W;
    for (int ox = 0; ox < SRC_W; ox++) {
      int sx = ox / sc, fx = ox % sc;
      int sx1 = (sx + 1 < w) ? sx + 1 : sx;
      int wx = (fx * 256) / sc, iwx = 256 - wx;
      uint16_t a = r0[sx], b = r0[sx1], e = r1[sx], f = r1[sx1];
      uint32_t rr = ((a >> 11) & 0x1F) * iwx * iwy + ((b >> 11) & 0x1F) * wx * iwy +
                    ((e >> 11) & 0x1F) * iwx * wy  + ((f >> 11) & 0x1F) * wx * wy;
      uint32_t gg = ((a >> 5) & 0x3F) * iwx * iwy + ((b >> 5) & 0x3F) * wx * iwy +
                    ((e >> 5) & 0x3F) * iwx * wy  + ((f >> 5) & 0x3F) * wx * wy;
      uint32_t bb = (a & 0x1F) * iwx * iwy + (b & 0x1F) * wx * iwy +
                    (e & 0x1F) * iwx * wy  + (f & 0x1F) * wx * wy;
      uint16_t px = (uint16_t)(((rr >> 16) << 11) | ((gg >> 16) << 5) | (bb >> 16));
      d[ox] = RPC_OUT(px);
    }
  }
}

// ---------------------------------------------------------------------------
// CACHE POR ESTADO
//
// La clave es (ambiente, Q, cajas). Dos profundidades que dan el mismo estado
// dan el mismo resultado bit a bit, asi que arrastrar el dedo dentro de un
// estado no cuesta NADA: se vuelve a volcar lo que ya hay.
// ---------------------------------------------------------------------------
struct Slot {
  uint16_t *px = nullptr;
  int atmos = -1, q = -1, b0 = -1, b1 = -1, b2 = -1;
  uint32_t used = 0;
};

static Slot coarse[RPC_COARSE_SLOTS];
static uint32_t useClock = 1;

static bool slotMatches(const Slot &s, int atmos, int q, const int *b) {
  return s.px && s.atmos == atmos && s.q == q &&
         s.b0 == b[0] && s.b1 == b[1] && s.b2 == b[2];
}

// Nivel grueso: ladera entera del recorrido, construida a demanda. Con 63
// estados posibles y RPC_COARSE_SLOTS huecos, arrastrar de punta a punta llena
// la cache y a la vuelta ya no calcula nada. Cuando no quedan huecos se
// reutiliza el menos reciente.
bool rpcDrag(uint16_t *out412, int atmos, float depth) {
  if (depth < 0.03f) {
    const uint16_t *s = RPC_SRC(atmos);
    for (int32_t i = 0; i < (int32_t)SRC_W * SRC_H; i++) out412[i] = RPC_OUT(s[i]);
    return true;
  }
  int q = rpcQForDepth(depth);
  int b[3]; boxesForGauss(rpcSigmaForDepth(depth, RPC_CW), b);

  int free_i = -1, lru = 0;
  for (int i = 0; i < RPC_COARSE_SLOTS; i++) {
    if (slotMatches(coarse[i], atmos, q, b)) {
      coarse[i].used = ++useClock;
      blitSmooth(coarse[i].px, RPC_CW, RPC_CH, out412);
      return true;
    }
    if (!coarse[i].px && free_i < 0) free_i = i;
    if (coarse[i].used < coarse[lru].used) lru = i;
  }
  int i = free_i;
  if (i < 0) i = lru;
  else if (!(coarse[i].px = (uint16_t *)RPC_ALLOC((size_t)RPC_CW * RPC_CH * 2))) {
    i = lru;                                  // sin memoria: se recicla
    if (!coarse[i].px) return false;
  }
  if (!reduceQS(LC, atmos, q, rpcSigmaForDepth(depth, RPC_CW), coarse[i].px)) return false;
  coarse[i].atmos = atmos; coarse[i].q = q;
  coarse[i].b0 = b[0]; coarse[i].b1 = b[1]; coarse[i].b2 = b[2];
  coarse[i].used = ++useClock;
  blitSmooth(coarse[i].px, RPC_CW, RPC_CH, out412);
  return true;
}

// Nivel fino: un solo hueco. Es lo que se mira cuando el aparato esta quieto, y
// mientras no cambie el estado no se recalcula.
static Slot still;

bool rpcStill(uint16_t *out412, int atmos, float depth) {
  if (depth < 0.03f) {
    const uint16_t *s = RPC_SRC(atmos);
    for (int32_t i = 0; i < (int32_t)SRC_W * SRC_H; i++) out412[i] = RPC_OUT(s[i]);
    return true;
  }
  int q = rpcQForDepth(depth);
  float sg = rpcSigmaForDepth(depth, RPC_W);
  int b[3]; boxesForGauss(sg, b);
  if (!slotMatches(still, atmos, q, b)) {
    if (!still.px) still.px = (uint16_t *)RPC_ALLOC((size_t)RPC_W * RPC_H * 2);
    if (!still.px) return false;
    if (!reduceQS(LF, atmos, q, sg, still.px)) return false;
    still.atmos = atmos; still.q = q;
    still.b0 = b[0]; still.b1 = b[1]; still.b2 = b[2];
  }
  blitN(still.px, RPC_W, RPC_H, out412);
  return true;
}

void rpcForget() {
  still.q = -1; still.atmos = -1;
  for (int i = 0; i < RPC_COARSE_SLOTS; i++) { coarse[i].q = -1; coarse[i].atmos = -1; }
}

// ---------------------------------------------------------------------------
// Respiracion
// ---------------------------------------------------------------------------
struct State { int q; float sigma; };

static State  states[RPC_STATES_MAX];
static uint16_t *frames[RPC_STATES_MAX];
static uint8_t buildOrder[RPC_STATES_MAX];
static bool   haveFrame[RPC_STATES_MAX];
static int  nStates = 0, nBuilt = 0, midState = 0;
static int  playLo = 0, playHi = 0;
static int  breathAtmos = -1;
static float breathLo = 0.30f, breathHi = 0.75f;

void rpcBreathFree() {
  for (int i = 0; i < RPC_STATES_MAX; i++) {
    if (frames[i]) { RPC_FREE(frames[i]); frames[i] = nullptr; }
    haveFrame[i] = false;
  }
  nStates = nBuilt = 0;
  breathAtmos = -1;
}

static void enumerateStates() {
  nStates = 0;
  int lastQ = -1, lastB[3] = { -1, -1, -1 };
  const int SAMPLES = 1024;
  for (int k = 0; k <= SAMPLES && nStates < RPC_STATES_MAX; k++) {
    float d = breathLo + (breathHi - breathLo) * (float)k / (float)SAMPLES;
    int q = rpcQForDepth(d);
    float sg = rpcSigmaForDepth(d, RPC_W);
    int b[3]; boxesForGauss(sg, b);
    if (q != lastQ || b[0] != lastB[0] || b[1] != lastB[1] || b[2] != lastB[2]) {
      states[nStates].q = q;
      states[nStates].sigma = sg;
      nStates++;
      lastQ = q; lastB[0] = b[0]; lastB[1] = b[1]; lastB[2] = b[2];
    }
  }
}

void rpcBreathSet(int atmos, float lo, float hi) {
  if (atmos == breathAtmos && lo == breathLo && hi == breathHi && nStates) return;
  rpcBreathFree();
  breathAtmos = atmos;
  breathLo = lo; breathHi = hi;
  enumerateStates();
  nBuilt = 0;

  // Se construye DESDE EL MEDIO HACIA FUERA, y hasta que el ciclo no esta
  // entero se muestra ese estado central. Asi, al soltar el aparato, lo primero
  // que aparece es practicamente la misma imagen que habia bajo el pulgar: la
  // respiracion empieza desde donde lo dejaste. Ciclar con el recorrido a
  // medias tampoco vale: la longitud cambia en cada fotograma y el vaiven sale
  // a tirones.
  midState = nStates / 2;
  playLo = 0; playHi = nStates - 1;
  int k = 0;
  for (int offs = 0; offs < nStates && k < nStates; offs++) {
    int a = midState - offs, b = midState + offs;
    if (offs == 0) { buildOrder[k++] = (uint8_t)midState; continue; }
    if (a >= 0 && k < nStates)      buildOrder[k++] = (uint8_t)a;
    if (b < nStates && k < nStates) buildOrder[k++] = (uint8_t)b;
  }
}

int rpcBreathReady() { return nBuilt; }
int rpcBreathTotal() { return nStates; }

bool rpcBreathBuild() {
  if (breathAtmos < 0 || nBuilt >= nStates) return false;
  int i = buildOrder[nBuilt];
  if (!frames[i]) frames[i] = (uint16_t *)RPC_ALLOC((size_t)RPC_W * RPC_H * 2);
  if (!frames[i]) {
    // Sin memoria para el ciclo entero. No es un fallo: se respira sobre el
    // tramo CONTIGUO que si esta, que por el orden de construccion rodea al
    // central. No se renumera nada -frames[] va indexado por estado- sino que
    // se acorta el recorrido.
    playLo = playHi = midState;
    while (playLo > 0 && haveFrame[playLo - 1]) playLo--;
    while (playHi < nStates - 1 && haveFrame[playHi + 1]) playHi++;
    nBuilt = nStates;
    return false;
  }
  if (!reduceQS(LF, breathAtmos, states[i].q, states[i].sigma, frames[i])) {
    nStates = nBuilt;
    return false;
  }
  haveFrame[i] = true;
  nBuilt++;
  return nBuilt < nStates;
}

bool rpcBreathFrame(uint16_t *out412, uint32_t now_ms, uint16_t ms_per_frame) {
  if (nBuilt < 1) return false;
  if (ms_per_frame < 1) ms_per_frame = 1;

  // Ciclo a medias: se sostiene el estado central y ya.
  if (nBuilt < nStates) {
    if (!haveFrame[midState]) return false;
    blitN(frames[midState], RPC_W, RPC_H, out412);
    return true;
  }

  // Ida y vuelta sobre [playLo, playHi]. Con n estados el ciclo tiene 2n-2
  // fotogramas: los extremos NO se repiten, o la respiracion se queda plana en
  // las puntas, que es justo donde se nota.
  int n = playHi - playLo + 1;
  if (n < 1) return false;
  int span = n > 1 ? (n - 1) * 2 : 1;
  uint32_t k = (now_ms / ms_per_frame) % (uint32_t)span;
  int i = (int)k;
  if (i >= n) i = span - i;
  blitN(frames[playLo + i], RPC_W, RPC_H, out412);
  return true;
}
