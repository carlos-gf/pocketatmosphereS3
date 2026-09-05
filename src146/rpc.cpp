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
#else
  #include <Arduino.h>
  #include "atmos.h"
  #include "field.h"          // FIELD_OUT: sin swap en Arduino_GFX
  #define RPC_ALLOC(n) ps_malloc(n)
  #define RPC_FREE(p)  free(p)
  #define RPC_SRC(a) (ATMOS[a].rgb)
  #define RPC_OUT(c) FIELD_OUT(c)
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
// que ordena ya son datos desenfocados; el error de ~0.1 grados no llega a
// mover un rango. Doce operaciones frente a los ~300 ciclos de atan2f, por
// 42436 pixeles y 30 estados.
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
// Buffers. Los indices caben en uint16 porque 206*206 = 42436 < 65536: eso
// parte por la mitad la memoria Y el trafico a PSRAM, que es lo que manda aqui.
// ---------------------------------------------------------------------------
static uint16_t *img = nullptr;      // 206x206 RGB565, la fuente reducida
static uint16_t *lum = nullptr;      // luminancia 0..65535
static uint16_t *hueT = nullptr;     // termino de tono, 0..1023
static int32_t  *hcos = nullptr;     // vector de tono pesado por croma
static int32_t  *hsin = nullptr;
static int32_t  *tmpA = nullptr;     // intermedios del desenfoque
static int32_t  *tmpB = nullptr;
static uint16_t *keyD = nullptr;     // clave de destino (luminancia desenfocada)
static uint16_t *keyH = nullptr;     // campo de tono, 0..65535
static uint16_t *band = nullptr;
static uint16_t *keyS = nullptr;     // clave de origen: banda<<10 | tono
static int32_t  *tmpC = nullptr;     // tercer intermedio: hacen falta dos campos
static uint16_t *ordS = nullptr, *ordD = nullptr, *ordT = nullptr, *ordX = nullptr;
static int srcFor = -1;              // que ambiente hay preparado

static bool ensureCommon() {
  if (!img)  img  = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!lum)  lum  = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!hueT) hueT = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!hcos) hcos = (int32_t  *)RPC_ALLOC(RPC_N * 4);
  if (!hsin) hsin = (int32_t  *)RPC_ALLOC(RPC_N * 4);
  if (!tmpA) tmpA = (int32_t  *)RPC_ALLOC(RPC_N * 4);
  if (!tmpB) tmpB = (int32_t  *)RPC_ALLOC(RPC_N * 4);
  if (!tmpC) tmpC = (int32_t  *)RPC_ALLOC(RPC_N * 4);
  if (!keyD) keyD = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!keyH) keyH = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!band) band = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!keyS) keyS = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!ordS) ordS = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!ordD) ordD = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!ordT) ordT = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!ordX) ordX = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  return img && lum && hueT && hcos && hsin && tmpA && tmpB && tmpC &&
         keyD && keyH && band && keyS && ordS && ordD && ordT && ordX;
}

// ---------------------------------------------------------------------------
// Preparacion por ambiente: reduce 412 -> 206 promediando 2x2, y de ahi saca
// luminancia, termino de tono y el vector de tono pesado por croma. Todo esto
// NO depende de la profundidad, asi que se hace una vez por fotografia.
// ---------------------------------------------------------------------------
static bool prepare(int atmos) {
  if (srcFor == atmos && img) return true;
  if (!ensureCommon()) return false;
  const uint16_t *s = RPC_SRC(atmos);

  for (int y = 0; y < RPC_H; y++) {
    for (int x = 0; x < RPC_W; x++) {
      int sx = x * RPC_SCALE, sy = y * RPC_SCALE;
      uint32_t rr = 0, gg = 0, bb = 0;
      for (int dy = 0; dy < RPC_SCALE; dy++)
        for (int dx = 0; dx < RPC_SCALE; dx++) {
          uint16_t c = s[(size_t)(sy + dy) * SRC_W + (sx + dx)];
          rr += (c >> 11) & 0x1F; gg += (c >> 5) & 0x3F; bb += c & 0x1F;
        }
      const uint32_t m = RPC_SCALE * RPC_SCALE;
      uint32_t r = rr / m, g = gg / m, b = bb / m;
      int32_t i = y * RPC_W + x;
      img[i] = (uint16_t)((r << 11) | (g << 5) | b);

      // Luminancia sobre las componentes 5/6/5 CRUDAS. Los pesos son
      // 0.2126*65535/31 = 449, 0.7152*65535/63 = 744, 0.0722*65535/31 = 153.
      // Poner aqui los pesos de 0..255 fue lo que desbordo a 16 bits y saturo
      // el campo la vez anterior; se encontro midiendo, no mirando.
      uint32_t Y = 449u * r + 744u * g + 153u * b;
      if (Y > 65535u) Y = 65535u;
      lum[i] = (uint16_t)Y;

      // HSV directo, sin cambiar de modo de color por pixel.
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
      hueT[i] = (uint16_t)(ht < 0 ? 0 : (ht > 1023 ? 1023 : ht));

      // El tono es un ANGULO: se promedia como vector o la media de dos rojos
      // profundos sale cian. Y se pesa por croma, para que los grises -que no
      // tienen tono que aportar- no arrastren el resultado.
      float wgt = ss * mx;
      float ang = 6.28318531f * hh;
      hcos[i] = (int32_t)(cosf(ang) * wgt * 4096.0f);
      hsin[i] = (int32_t)(sinf(ang) * wgt * 4096.0f);
    }
  }
  srcFor = atmos;
  return true;
}

// ---------------------------------------------------------------------------
// Desenfoque de caja, sumas corridas: O(1) por pixel sea cual sea sigma. Tres
// pasadas por eje aproximan una gaussiana con un error de pocos por ciento.
// ---------------------------------------------------------------------------
static void boxH(const int32_t *a, int32_t *d, int w, int h, int r) {
  if (r > (w - 1) / 2) r = (w - 1) / 2;
  if (r < 1) { memcpy(d, a, (size_t)w * h * 4); return; }
  const int32_t div = 2 * r + 1;
  for (int y = 0; y < h; y++) {
    const int32_t *row = a + (size_t)y * w;
    int32_t *dst = d + (size_t)y * w;
    int64_t acc = (int64_t)row[0] * (r + 1);
    for (int i = 1; i <= r; i++) acc += row[i];
    for (int x = 0; x < w; x++) {
      dst[x] = (int32_t)((acc + div / 2) / div);
      int ia = x - r, ib = x + r + 1;
      acc += row[ib < w ? ib : w - 1];
      acc -= row[ia > 0 ? ia : 0];
    }
  }
}

static void boxV(const int32_t *a, int32_t *d, int w, int h, int r) {
  if (r > (h - 1) / 2) r = (h - 1) / 2;
  if (r < 1) { memcpy(d, a, (size_t)w * h * 4); return; }
  const int32_t div = 2 * r + 1;
  for (int x = 0; x < w; x++) {
    int64_t acc = (int64_t)a[x] * (r + 1);
    for (int i = 1; i <= r; i++) acc += a[(size_t)i * w + x];
    for (int y = 0; y < h; y++) {
      d[(size_t)y * w + x] = (int32_t)((acc + div / 2) / div);
      int ia = y - r, ib = y + r + 1;
      acc += a[(size_t)(ib < h ? ib : h - 1) * w + x];
      acc -= a[(size_t)(ia > 0 ? ia : 0) * w + x];
    }
  }
}

// Desenfoca tmpA en sitio, usando tmpB de ida y vuelta.
static void blur3(int32_t *buf, int32_t *scratch, const int *boxes) {
  for (int i = 0; i < 3; i++) {
    int r = (boxes[i] - 1) / 2;
    if (r < 1) continue;
    boxH(buf, scratch, RPC_W, RPC_H, r);
    boxV(scratch, buf, RPC_W, RPC_H, r);
  }
}

// ---------------------------------------------------------------------------
// Radix estable de dos digitos de 8 bits sobre indices de 16 bits.
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
// UNA reduccion, a 206x206, con Q y sigma dados por separado. Escribe en dst
// (RGB565, RPC_N). Es el nucleo: todo lo demas es cache y calendario.
// ---------------------------------------------------------------------------
static bool reduceQS(int atmos, int Q, float sigma, uint16_t *dst) {
  if (!prepare(atmos)) return false;
  const int32_t n = RPC_N;
  int boxes[3];
  boxesForGauss(sigma, boxes);

  // --- clave de destino: luminancia desenfocada ------------------------------
  for (int32_t i = 0; i < n; i++) tmpA[i] = lum[i];
  blur3(tmpA, tmpB, boxes);
  for (int32_t i = 0; i < n; i++) {
    int32_t v = tmpA[i];
    keyD[i] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
  }

  // --- campo de tono: los dos vectores, desenfocados, y su angulo ------------
  // Hacen falta los DOS campos a la vez para el atan2, asi que el coseno se
  // aparca en tmpC mientras se desenfoca el seno.
  for (int32_t i = 0; i < n; i++) tmpA[i] = hcos[i];
  blur3(tmpA, tmpB, boxes);
  memcpy(tmpC, tmpA, (size_t)n * 4);         // tmpC = coseno desenfocado
  for (int32_t i = 0; i < n; i++) tmpA[i] = hsin[i];
  blur3(tmpA, tmpB, boxes);                  // tmpA = seno desenfocado
  for (int32_t i = 0; i < n; i++) {
    float t = atan2turns((float)tmpA[i], (float)tmpC[i]);
    int v = (int)(t * 65535.0f + 0.5f);
    keyH[i] = (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v));
  }

  // --- clave de origen: banda de luminancia + tono dentro de la banda --------
  // Se clampea la BANDA, no el cociente: con el cociente, un blanco puro se
  // colaba como banda == Q y se salia de la tabla.
  int32_t counts[64];
  int nb = Q > 64 ? 64 : Q;
  memset(counts, 0, sizeof(int32_t) * nb);
  for (int32_t i = 0; i < n; i++) {
    float ln = lum[i] * (1.0f / 65535.0f) + RPC_TEXTURE * (hashNoise((uint32_t)i) - 0.5f);
    int bi = (int)floorf(ln * (float)Q);
    if (bi < 0) bi = 0; else if (bi > nb - 1) bi = nb - 1;
    band[i] = (uint16_t)bi;
    counts[bi]++;
    // banda en los 6 bits altos, tono en los 10 bajos: el tono nunca alcanza 1,
    // asi que las bandas no se mezclan.
    keyS[i] = (uint16_t)(((uint16_t)bi << 10) | (hueT[i] & 0x3FF));
  }

  // OJO con los alias: radix16 escribe tmp leyendo in, asi que tmp NO puede ser
  // in. (out == in si vale: para cuando se escribe out, tmp ya tiene el dato.)
  for (int32_t i = 0; i < n; i++) ordS[i] = (uint16_t)i;
  radix16(ordS, ordT, ordX, n, keyS);   // ordT = origen ordenado por banda+tono
  radix16(ordS, ordD, ordX, n, keyD);   // ordD = destino ordenado por el campo

  // --- colocacion del color dentro de cada banda -----------------------------
  // Sin esto, el tono se reparte a lo largo del gradiente fino de luminancia y
  // cada color sale en hilos finos esparcidos por la imagen: a distancia de
  // lectura se promedian con el fondo y el color DESAPARECE. Cada banda se
  // queda con la misma region de destino que le daba la escalera -la rebanada
  // esta definida por RANGO, asi que la legibilidad no se toca- pero las
  // posiciones de dentro se reordenan por el campo de tono.
  int32_t off = 0;
  for (int k = 0; k < nb; k++) {
    int32_t c = counts[k];
    if (c > 1) radix16(ordD + off, ordD + off, ordX, c, keyH);
    off += c;
  }

  for (int32_t k = 0; k < n; k++) dst[ordD[k]] = img[ordT[k]];
  return true;
}

// ---------------------------------------------------------------------------
// Volcado 206 -> 412 por vecino mas proximo. Medido: es lo que devuelve la
// energia de escala fina de la referencia (5.99 frente a 5.89); con bilineal
// cae a 1.92, porque la interpolacion se come el grano.
// ---------------------------------------------------------------------------
static void blit2x(const uint16_t *s, uint16_t *out412) {
  for (int y = 0; y < RPC_H; y++) {
    const uint16_t *sr = s + (size_t)y * RPC_W;
    uint16_t *d0 = out412 + (size_t)(y * 2) * SRC_W;
    uint16_t *d1 = d0 + SRC_W;
    for (int x = 0; x < RPC_W; x++) {
      uint16_t c = RPC_OUT(sr[x]);
      d0[x * 2] = c; d0[x * 2 + 1] = c;
      d1[x * 2] = c; d1[x * 2 + 1] = c;
    }
  }
}

// ---------------------------------------------------------------------------
// Fotograma suelto
// ---------------------------------------------------------------------------
static uint16_t *stillBuf = nullptr;

bool rpcStill(uint16_t *out412, int atmos, float depth) {
  // Profundidad cero es la fotografia, intacta: sin coste y sin dudas.
  if (depth < 0.03f) {
    const uint16_t *s = RPC_SRC(atmos);
    for (int32_t i = 0; i < (int32_t)SRC_W * SRC_H; i++) out412[i] = RPC_OUT(s[i]);
    return true;
  }
  if (!stillBuf) stillBuf = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!stillBuf) return false;
  if (!reduceQS(atmos, rpcQForDepth(depth), rpcSigmaForDepth(depth, RPC_W), stillBuf))
    return false;
  blit2x(stillBuf, out412);
  return true;
}

// ---------------------------------------------------------------------------
// Respiracion
// ---------------------------------------------------------------------------
struct State { int q; float sigma; };

static State  states[RPC_STATES_MAX];
static uint16_t *frames[RPC_STATES_MAX];
static uint8_t buildOrder[RPC_STATES_MAX];   // por cual se empieza
static bool   haveFrame[RPC_STATES_MAX];
static int  nStates = 0, nBuilt = 0, midState = 0;
static int  playLo = 0, playHi = 0;   // tramo que se reproduce (todo, salvo si falto memoria)
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

// Enumera los estados DISTINTOS del recorrido: un fotograma por cuanto de Q o
// de anchura de caja. Ni fotogramas muertos ni saltos dobles.
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
  // respiracion empieza desde donde lo dejaste, no desde un extremo. Ciclar con
  // el ciclo a medias tampoco vale -la longitud cambia en cada fotograma y el
  // vaiven sale a tirones-.
  midState = nStates / 2;
  playLo = 0; playHi = nStates - 1;
  int k = 0;
  for (int off = 0; off < nStates && k < nStates; off++) {
    int a = midState - off, b = midState + off;
    if (off == 0) { buildOrder[k++] = (uint8_t)midState; continue; }
    if (a >= 0 && k < nStates)      buildOrder[k++] = (uint8_t)a;
    if (b < nStates && k < nStates) buildOrder[k++] = (uint8_t)b;
  }
}

int rpcBreathReady() { return nBuilt; }
int rpcBreathTotal() { return nStates; }

bool rpcBreathBuild() {
  if (breathAtmos < 0 || nBuilt >= nStates) return false;
  int i = buildOrder[nBuilt];
  if (!frames[i]) frames[i] = (uint16_t *)RPC_ALLOC(RPC_N * 2);
  if (!frames[i]) {
    // Sin memoria para el ciclo entero. No es un fallo: se respira con el tramo
    // CONTIGUO que si esta, que por el orden de construccion rodea al central.
    // No se renumeran los estados -frames[] va indexado por estado- sino que se
    // acorta el recorrido.
    playLo = playHi = midState;
    while (playLo > 0 && haveFrame[playLo - 1]) playLo--;
    while (playHi < nStates - 1 && haveFrame[playHi + 1]) playHi++;
    nBuilt = nStates;          // se da por terminado: no hay mas que construir
    return false;
  }
  if (!reduceQS(breathAtmos, states[i].q, states[i].sigma, frames[i])) {
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

  // Ciclo a medias: se sostiene el estado central y ya. Nada de vaiven sobre un
  // recorrido que aun esta creciendo.
  if (nBuilt < nStates) {
    if (!haveFrame[midState]) return false;
    blit2x(frames[midState], out412);
    return true;
  }

  // Ida y vuelta sobre [playLo, playHi]. Con n estados el ciclo tiene 2n-2
  // fotogramas: los extremos NO se repiten, o la respiracion se queda plana en
  // las puntas -que es justo donde se nota-.
  int n = playHi - playLo + 1;
  if (n < 1) return false;
  int span = n > 1 ? (n - 1) * 2 : 1;
  uint32_t k = (now_ms / ms_per_frame) % (uint32_t)span;
  int i = (int)k;
  if (i >= n) i = span - i;
  blit2x(frames[playLo + i], out412);
  return true;
}
