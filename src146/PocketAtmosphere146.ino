// ---------------------------------------------------------------------------
// POCKET ATMOSPHERE
// M5Stack CoreS3 (ESP32-S3, 320x240 IPS tactil, BMI270, BM8563, altavoz, mic)
//
// Un aparato de bolsillo que sostiene seis campos atmosfericos y deja que quien
// lo tiene en la mano decida cuanta reduccion quiere y que es lo que esta viendo.
//
// Tres decisiones de diseno, y las tres son deliberadas:
//
//   1. El aparato NUNCA nombra sus propios campos. Se llaman I..VI. Poner
//      "puerto al amanecer" debajo cerraria la lectura antes de que empiece:
//      la aportacion del espectador es el tema, no un efecto secundario.
//
//   2. La profundidad de reduccion esta en el pulgar, no en el firmware. Se
//      arrastra en vertical sobre el propio campo y se mueve en vivo. La
//      pregunta "donde esta la banda entre aburrimiento y sobreesfuerzo" se la
//      responde cada persona con la mano, y el aparato apunta donde se queda.
//
//   3. Lo que se guarda son datos del espectador, no del aparato: las palabras
//      que le ha dado la gente (con la profundidad a la que se las dio) y el
//      tiempo que cada cual pasa en cada profundidad. Se vuelca por USB con DUMP.
//
// Librerias: M5Unified + M5GFX. Todo el hardware pasa por board_cores3.h, que
// mantiene los nombres que usaba la version de la Waveshare.
// Placa: M5Stack CoreS3 | Flash 16MB | PSRAM enabled | USB CDC On Boot enabled
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <math.h>
#include "board_ws146.h"
#include "SensorQMI8658.hpp"
#include <SensorPCF85063.hpp>
#include "atmos.h"
#include "field.h"
#include "store.h"
#include "buzz.h"
#if IMAGE_FIELDS
#include "rpc.h"
#endif

#define FW_VERSION "0.4c-146"

// QSPI -> SPD2010. El reset del panel cuelga del expansor, asi que aqui va
// GFX_NOT_DEFINED y se hace a mano antes de begin().
static Arduino_DataBus *bus =
    new Arduino_ESP32QSPI(QSPI_CS, QSPI_SCK, QSPI_D0, QSPI_D1, QSPI_D2, QSPI_D3);
static Arduino_GFX *panel = new Arduino_SPD2010(bus, GFX_NOT_DEFINED);
static PACanvas canvasObj(panel);
static PACanvas *gfx = &canvasObj;

static TouchShim touch;
static SensorQMI8658 imu;
static SensorPCF85063 rtc;
static bool imuOk = false, rtcOk = false;

// ---------------------------------------------------------------------------
// estado
// ---------------------------------------------------------------------------

enum Mode : uint8_t { M_FIELD = 0, M_NAMES, M_BAND, M_INDEX, M_SET, M_KEY };
static Mode mode = M_FIELD;

static FieldState fs;
static float depthByAtmos[ATMOS_COUNT];

static uint32_t hudUntil = 0;       // la interfaz se retira sola
static bool showDepthBig = false;   // lectura grande mientras se arrastra
static uint32_t lastFrame = 0, lastDwell = 0;
static bool screenOff = false;

// teclado
static char nameBuf[NAME_LEN] = "";
static uint8_t nameLen = 0;

// revelacion tras nombrar
static uint32_t revealUntil = 0;

// gestos
static bool touchDown = false;
static int16_t gx0, gy0, gxl, gyl;
static uint32_t gStart = 0;
static float gDepth0 = 0;
static uint8_t gAxis = 0;  // 0 sin decidir, 1 vertical (profundidad), 2 horizontal
static bool gLongFired = false;


// ---------------------------------------------------------------------------
// utilidades de dibujo
// ---------------------------------------------------------------------------

static uint16_t inkOn(const FieldState &s) { return fieldIsLight(s) ? 0x2124 : 0xE73C; }
static uint16_t groundOf(const FieldState &s) {
  uint32_t c = ATMOS[s.atmos].pal24[1];
  return C24(c);
}
static uint16_t inkOfGround(const FieldState &s) {
  uint32_t c = ATMOS[s.atmos].pal24[13];
  return C24(c);
}

static void textAt(int x, int y, const char *s, uint16_t col, uint8_t size) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(s);
}
static void textMid(int cx, int y, const char *s, uint16_t col, uint8_t size) {
  textAt(cx - (int)strlen(s) * 3 * size, y, s, col, size);
}

// En un disco no hay esquinas. Los controles viven en los extremos del eje
// horizontal -donde el circulo da mas holgura- y el menu del campo, abajo en el
// centro. Las zonas de toque son circulos, no rectangulos: en una pantalla
// redonda un rectangulo de esquina cae medio fuera del cristal.
#define NAV_BACK_X 40
#define NAV_NEXT_X 372
#define NAV_AXIS_Y 206
#define NAV_MENU_X 206
#define NAV_MENU_Y 352
#define NAV_R 46

static bool inDisc(int x, int y, int cx, int cy, int r) {
  int dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

static void drawCorner(uint16_t col, bool back) {
  if (back) {
    int x = NAV_BACK_X, y = NAV_AXIS_Y;
    for (int i = 0; i < 9; i++) gfx->drawFastHLine(x - 5 + i, y - i, 1, col);
    for (int i = 0; i < 9; i++) gfx->drawFastHLine(x - 5 + i, y + i, 1, col);
    gfx->drawFastHLine(x - 5, y, 20, col);
  } else {
    for (int i = 0; i < 3; i++) gfx->fillRect(NAV_MENU_X - 10, NAV_MENU_Y - 7 + i * 7, 20, 3, col);
  }
}

static void drawNextCorner(uint16_t col) {
  int x = NAV_NEXT_X, y = NAV_AXIS_Y;
  for (int i = 0; i < 9; i++) gfx->drawFastHLine(x + 5 - i, y - i, 1, col);
  for (int i = 0; i < 9; i++) gfx->drawFastHLine(x + 5 - i, y + i, 1, col);
  gfx->drawFastHLine(x - 15, y, 20, col);
}

static bool inCorner(int x, int y) {
  return inDisc(x, y, NAV_BACK_X, NAV_AXIS_Y, NAV_R) ||
         inDisc(x, y, NAV_MENU_X, NAV_MENU_Y, NAV_R);
}
static bool inNextCorner(int x, int y) { return inDisc(x, y, NAV_NEXT_X, NAV_AXIS_Y, NAV_R); }

static void drawBattery(int x, int y, uint16_t col) {
  int pct = boardBatteryPct();
  gfx->drawRect(x, y, 18, 9, col);
  gfx->fillRect(x + 18, y + 3, 2, 3, col);
  gfx->fillRect(x + 2, y + 2, (16 * pct) / 100, 5, col);
  if (boardCharging()) {
    // un rayo dentro de la pila: se esta llenando
    gfx->fillRect(x + 9, y + 1, 2, 4, col ^ 0xFFFF);
    gfx->fillRect(x + 7, y + 4, 2, 4, col ^ 0xFFFF);
    gfx->drawFastHLine(x + 8, y + 4, 3, col ^ 0xFFFF);
  }
}

// ---------------------------------------------------------------------------
// pantalla principal: el campo
// ---------------------------------------------------------------------------

#if IMAGE_FIELDS
// ---------------------------------------------------------------------------
// EL CAMPO, FOTOGRAMA A FOTOGRAMA
//
// La primera version recalculaba la reduccion entera en cada vuelta de loop(),
// incluso cuando no habia cambiado nada. Eso es lo que hacia que el campo fuera
// a trompicones mientras los menus -que no calculan nada- iban finos: el
// sintoma senalaba el sitio exacto.
//
// Ahora hay tres regimenes y ninguno recalcula de mas:
//
//   arrastrando   nivel grueso (103 px, la cuarta parte del trabajo) con una
//                 cache de TODO el recorrido. Ademas, Q y las anchuras de las
//                 cajas son enteras, asi que mover el dedo dentro de un mismo
//                 estado da el mismo resultado bit a bit y no cuesta nada.
//   quieto        nivel fino (206 px), un solo hueco de cache. Mientras no
//                 cambie el estado esto es un volcado.
//   respirando    el ciclo cacheado; tambien un volcado.
//
// La unica reduccion fina que queda es la del instante en que se suelta el
// dedo: una sola, y despues el aparato ya no calcula nada hasta que lo toques.
// ---------------------------------------------------------------------------
#define BREATH_SPAN 0.22f     // +- alrededor de la profundidad elegida
#define BREATH_MS   330       // por fotograma: el ciclo entero dura unos 17 s
#define BREATH_WAIT 1500      // hay que soltarlo este rato para que arranque
static uint32_t lastInteract = 0;

static void renderFieldPixels(uint32_t now) {
  uint16_t *fb = gfx->getFramebuffer();

  // Con el dedo encima manda la respuesta, no la resolucion.
  if (touchDown) { rpcDrag(fb, fs.atmos, fs.depth); return; }

  bool idle = storeSettings().breathe && (now - lastInteract > BREATH_WAIT);
  if (!idle) { rpcStill(fb, fs.atmos, fs.depth); return; }

  float lo = fs.depth - BREATH_SPAN, hi = fs.depth + BREATH_SPAN;
  if (lo < 0.05f) { hi += 0.05f - lo; lo = 0.05f; }
  if (hi > 0.98f) { lo -= hi - 0.98f; hi = 0.98f; }
  if (lo < 0.05f) lo = 0.05f;
  rpcBreathSet(fs.atmos, lo, hi);
  rpcBreathBuild();                      // como mucho un estado por fotograma
  if (!rpcBreathFrame(fb, now, BREATH_MS)) rpcStill(fb, fs.atmos, fs.depth);
}
#endif

static void renderField(uint32_t now) {
#if IMAGE_FIELDS
  renderFieldPixels(now);
#else
  fieldRender(gfx->getFramebuffer(), fs);
#endif
  fieldMaskCircle(gfx->getFramebuffer(), 0);   // fuera del disco no hay pantalla

  bool hud = (now < hudUntil) || showDepthBig;
  uint16_t ink = inkOn(fs);
  // La esquina se dibuja SIEMPRE, tambien con la interfaz retirada. Es la unica
  // salida que hay, y una salida que desaparece no es una salida.
  drawCorner(ink, false);

  if (revealUntil && now < revealUntil) {
    // tras nombrar, si el ajuste esta activo, se dice de donde venia
    const char *src = ATMOS[fs.atmos].source;
    gfx->fillRect(60, 176, 292, 44, groundOf(fs));
    textMid(LCD_CX, 184, "it was made from", inkOfGround(fs), 1);
    textMid(LCD_CX, 202, src, inkOfGround(fs), 1);
  } else if (revealUntil && now >= revealUntil) {
    revealUntil = 0;
  }

  if (!hud) return;

  textMid(LCD_CX, 74, ATMOS_NUMERAL[fs.atmos], ink, 2);
  drawBattery(LCD_CX - 9, 40, ink);

  // barra de profundidad: linea fina abajo, llena hasta el valor actual
  int by = 306, bx = 96, bw = 220;
  gfx->drawFastHLine(bx, by, bw, ink);
  int px = bx + (int)(bw * fs.depth);
  gfx->fillRect(px - 1, by - 5, 3, 11, ink);

  char lab[24];
  snprintf(lab, sizeof lab, "reduction %.2f", fs.depth);
  textMid(LCD_CX, by + 12, lab, ink, 1);

  if (showDepthBig) {
    snprintf(lab, sizeof lab, "%.2f", fs.depth);
    textMid(LCD_CX, 178, lab, ink, 5);
    snprintf(lab, sizeof lab, "%d colours", fieldLevels(fs.depth));
    textMid(LCD_CX, 232, lab, ink, 1);
  } else {
    uint8_t n = storeNameCount(fs.atmos);
    if (n) {
      snprintf(lab, sizeof lab, "%u names", (unsigned)n);
      textMid(LCD_CX, 112, lab, ink, 1);
    } else {
      textMid(LCD_CX, 112, "hold to name it", ink, 1);
    }
  }
}

// ---------------------------------------------------------------------------
// pantalla: los nombres que le ha dado la gente
// ---------------------------------------------------------------------------

static void renderNames() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);

  char h[24];
  snprintf(h, sizeof h, "NAMED  %s", ATMOS_NUMERAL[fs.atmos]);
  textMid(LCD_CX, 74, h, ink, 2);
  drawCorner(ink, true);
  drawNextCorner(ink);

  uint8_t n = storeNameCount(fs.atmos);
  if (n == 0) {
    textMid(LCD_CX, 190, "no one has named", ink, 1);
    textMid(LCD_CX, 210, "this one yet", ink, 1);
  } else {
    // el eje de la derecha es la profundidad a la que se dijo cada palabra
    textAt(228, 104, "light", ink, 1);
    textAt(300, 104, "deep", ink, 1);
    // Cada fila: la palabra, y a que profundidad se dijo. Ver la lista completa
    // ES el punto: la divergencia deja de ser un numero y se vuelve legible.
    for (uint8_t i = 0; i < n && i < 10; i++) {
      const NameRec &r = storeName(fs.atmos, n - 1 - i);
      int y = 124 + i * 20;
      textAt(104, y, r.word, ink, 1);
      int bx = 228, bw = 88;
      gfx->drawFastHLine(bx, y + 4, bw, bg == 0 ? ink : ink);
      int px = bx + (r.depth * bw) / 255;
      gfx->fillRect(px - 1, y, 3, 9, ink);
    }
  }
  textMid(LCD_CX, 330, "arrows: back / next", ink, 1);
}

// ---------------------------------------------------------------------------
// pantalla: tu banda
// ---------------------------------------------------------------------------

static void renderBand() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_CX, 74, "YOUR BAND", ink, 2);
  drawCorner(ink, true);
  drawNextCorner(ink);
  textMid(LCD_CX, 100, "where you settle", ink, 1);

  const uint32_t *b = storeBand();
  uint32_t peak = 1;
  for (int i = 0; i < BAND_BUCKETS; i++)
    if (b[i] > peak) peak = b[i];

  int x0 = 92, w = 228, base = 288, hmax = 140;
  int bw = w / BAND_BUCKETS;
  for (int i = 0; i < BAND_BUCKETS; i++) {
    int hgt = (int)((uint64_t)b[i] * hmax / peak);
    if (hgt < 1 && b[i]) hgt = 1;
    gfx->fillRect(x0 + i * bw + 1, base - hgt, bw - 2, hgt, ink);
  }
  gfx->drawFastHLine(x0, base + 1, w, ink);
  textAt(x0, base + 8, "light", ink, 1);
  textAt(x0 + w - 30, base + 8, "deep", ink, 1);

  char s[40];
  float m = storeBandMean();
  uint32_t tot = storeBandTotal();
  if (m < 0) {
    textMid(LCD_CX, base + 22, "not enough time yet", ink, 1);
  } else {
    int px = x0 + (int)(m * w);
    gfx->fillRect(px - 1, base - hmax - 8, 3, 8, ink);
    snprintf(s, sizeof s, "mean %.2f", m);
    textMid(LCD_CX, base + 18, s, ink, 1);
    snprintf(s, sizeof s, "%lum %lus held", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    textMid(LCD_CX, base + 32, s, ink, 1);
  }
  textMid(LCD_CX, 330, "arrows: back / next", ink, 1);
}

// ---------------------------------------------------------------------------
// pantalla: la coleccion
// ---------------------------------------------------------------------------

// 320x240 es apaisada: la rejilla gira a 3 x 2 y las muestras se ensanchan.
#define IDX_COLS 3
#define IDX_ROWS 2
#define IDX_W 88
#define IDX_H 68
#define IDX_GAPX 12
#define IDX_GAPY 30
#define IDX_X ((LCD_WIDTH - IDX_COLS * IDX_W - (IDX_COLS - 1) * IDX_GAPX) / 2)
#define IDX_Y 118

static void renderIndex() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_CX, 74, "SIX FIELDS", ink, 2);
  drawCorner(ink, true);
  drawNextCorner(ink);

  for (int i = 0; i < ATMOS_COUNT; i++) {
    int c = i % IDX_COLS, r = i / IDX_COLS;
    int x = IDX_X + c * (IDX_W + IDX_GAPX), y = IDX_Y + r * (IDX_H + IDX_GAPY);
#if IMAGE_FIELDS
    // Miniatura HORNEADA. Antes esto reducia los seis campos en vivo, seis
    // reducciones enteras por fotograma, y la pantalla se quedaba clavada.
    // Una hoja de contactos no necesita ser el fotograma exacto; necesita
    // aparecer. Lo que si necesita es estar REDUCIDA: una miniatura con la
    // fotografia enseñaria justo lo que el aparato se niega a enseñar.
    // De los peldanos horneados se coge el mas cercano a la profundidad que
    // ese campo recuerda, asi que la rejilla dice tambien donde lo dejo cada
    // cual.
    int lvl = 0;
    float best = 9.0f;
    for (int k = 0; k < IMG_TH_LEVELS; k++) {
      float e = fabsf(depthByAtmos[i] - IMG_TH_DEPTH[k]);
      if (e < best) { best = e; lvl = k; }
    }
    const uint16_t *th = ATMOS[i].thumb + (size_t)lvl * IMG_TH_W * IMG_TH_H;
    uint16_t *fb = gfx->getFramebuffer();
    for (int ty = 0; ty < IDX_H; ty++) {
      const uint16_t *sr = th + (size_t)ty * IMG_TH_W;
      uint16_t *dr = fb + (size_t)(y + ty) * LCD_WIDTH + x;
      for (int tx = 0; tx < IDX_W; tx++) dr[tx] = FIELD_OUT(sr[tx]);
    }
#else
    FieldState s = fs;
    s.atmos = i;
    s.depth = depthByAtmos[i];
    s.t = fs.t * 0.6f + i * 7.3f;
    fieldSwatch(gfx->getFramebuffer(), LCD_WIDTH, LCD_HEIGHT, x, y, IDX_W, IDX_H, s);
#endif
    if (i == fs.atmos) gfx->drawRect(x - 2, y - 2, IDX_W + 4, IDX_H + 4, ink);
    char lab[16];
    snprintf(lab, sizeof lab, "%s  %u", ATMOS_NUMERAL[i], (unsigned)storeNameCount(i));
    textAt(x, y + IDX_H + 4, lab, ink, 1);
  }
  textMid(LCD_CX, 330, "tap one", ink, 1);
}

// ---------------------------------------------------------------------------
// pantalla: ajustes
// ---------------------------------------------------------------------------

struct Row { int y; const char *label; };
static const Row SET_ROWS[] = {
  { 122, "hour" }, { 152, "minute" }, { 182, "sound" },
  { 212, "reveal source" }, { 242, "breathing" }, { 272, "brightness" },
};
#define SET_N 6
static int setHour = 12, setMin = 0;

static void renderSettings() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_CX, 74, "SETTINGS", ink, 2);
  drawCorner(ink, true);
  drawNextCorner(ink);

  Settings &st = storeSettings();
  char v[24];
  for (int i = 0; i < SET_N; i++) {
    textAt(104, SET_ROWS[i].y, SET_ROWS[i].label, ink, 1);
    switch (i) {
      case 0: snprintf(v, sizeof v, "%02d", setHour); break;
      case 1: snprintf(v, sizeof v, "%02d", setMin); break;
      case 2: snprintf(v, sizeof v, "%s", st.sound ? "on" : "off"); break;
      case 3: snprintf(v, sizeof v, "%s", st.reveal ? "on" : "off"); break;
      case 4: snprintf(v, sizeof v, "%s", st.breathe ? "on" : "off"); break;
      default: snprintf(v, sizeof v, "%u", (unsigned)st.bright); break;
    }
    textAt(244, SET_ROWS[i].y, v, ink, 1);
    gfx->drawRect(210, SET_ROWS[i].y - 6, 20, 20, ink);
    textAt(216, SET_ROWS[i].y, "-", ink, 1);
    gfx->drawRect(286, SET_ROWS[i].y - 6, 20, 20, ink);
    textAt(292, SET_ROWS[i].y, "+", ink, 1);
  }
  textMid(LCD_CX, 306, "USB: DUMP  BAND  TEST", ink, 1);
  char fw[32];
  snprintf(fw, sizeof fw, "Pocket Atmosphere v%s", FW_VERSION);
  textMid(LCD_CX, 324, fw, ink, 1);
}

static void settingsTap(int16_t x, int16_t y) {
  Settings &st = storeSettings();
  for (int i = 0; i < SET_N; i++) {
    if (y < SET_ROWS[i].y - 8 || y > SET_ROWS[i].y + 14) continue;
    int dir = (x >= 280) ? +1 : (x >= 204 && x <= 236) ? -1 : 0;
    if (!dir) return;
    switch (i) {
      case 0: setHour = (setHour + dir + 24) % 24; break;
      case 1: setMin = (setMin + dir + 60) % 60; break;
      case 2: st.sound = !st.sound; buzzSetEnabled(st.sound); break;
      case 3: st.reveal = !st.reveal; break;
      case 4: st.breathe = !st.breathe; break;
      default:
        st.bright = (uint8_t)constrain((int)st.bright + dir * 15, 20, 255);
        boardBrightness(st.bright);
        break;
    }
    if (i <= 1 && rtcOk) rtc.setDateTime(RTC_DateTime(2026, 1, 1, setHour, setMin, 0));
    storeSaveSettings();
    buzzPing(880, 30);
    return;
  }
}

// ---------------------------------------------------------------------------
// teclado para nombrar
// ---------------------------------------------------------------------------

static const char *KB_ROWS[4] = { "ABCDEFG", "HIJKLMN", "OPQRSTU", "VWXYZ -" };
#define KB_X 73
#define KB_Y 172
#define KB_KW 38
#define KB_KH 32

static void renderKeyboard() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_CX, 74, "WHAT IS IT?", ink, 2);
  drawCorner(ink, true);   // salir sin nombrar: no todo el mundo quiere hacerlo
  textMid(LCD_CX, 100, "your word, not ours", ink, 1);

  gfx->drawRect(76, 122, 260, 34, ink);
  textAt(88, 132, nameBuf, ink, 2);

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 7; c++) {
      char ch = KB_ROWS[r][c];
      int x = KB_X + c * KB_KW, y = KB_Y + r * KB_KH;
      gfx->drawRect(x, y, KB_KW - 2, KB_KH - 2, ink);
      char s[2] = { ch, 0 };
      textAt(x + 12, y + 10, s, ink, 1);
    }
  }
  int y = KB_Y + 4 * KB_KH;
  gfx->drawRect(KB_X, y + 6, 116, 30, ink);
  textAt(KB_X + 40, y + 16, "DEL", ink, 1);
  gfx->drawRect(KB_X + 150, y + 6, 116, 30, ink);
  textAt(KB_X + 196, y + 16, "OK", ink, 1);
}

static void keyboardTap(int16_t x, int16_t y) {
  int r = (y - KB_Y) / KB_KH, c = (x - KB_X) / KB_KW;
  if (r >= 0 && r < 4 && c >= 0 && c < 7) {
    if (nameLen < NAME_LEN - 1) {
      nameBuf[nameLen++] = KB_ROWS[r][c];
      nameBuf[nameLen] = 0;
      buzzPing(1200, 22);
    }
    return;
  }
  int by = KB_Y + 4 * KB_KH + 6;
  if (y >= by && y <= by + 30) {
    if (x < KB_X + 116) {
      if (nameLen) nameBuf[--nameLen] = 0;
      buzzPing(600, 22);
    } else if (x >= KB_X + 150) {
      if (nameLen) {
        uint32_t e = 0;
        if (rtcOk) {
          RTC_DateTime t = rtc.getDateTime();
          e = (uint32_t)t.getHour() * 3600 + t.getMinute() * 60;
        }
        storeAddName(fs.atmos, nameBuf, fs.depth, e);
        buzzPing(1400, 90);
        if (storeSettings().reveal) revealUntil = millis() + 3500;
      }
      mode = M_FIELD;
      hudUntil = millis() + 2500;
    }
  }
}

// ---------------------------------------------------------------------------
// entrada
// ---------------------------------------------------------------------------

static void openKeyboard() {
  nameBuf[0] = 0;
  nameLen = 0;
  mode = M_KEY;
  buzzPing(700, 60);
}

static void nextMode() {
  switch (mode) {
    case M_FIELD: mode = M_NAMES; break;
    case M_NAMES: mode = M_BAND; break;
    case M_BAND: mode = M_INDEX; break;
    case M_INDEX: mode = M_SET; break;
    default: mode = M_FIELD; hudUntil = millis() + 2000; break;
  }
  buzzPing(990, 26);
}

static void handleTouch(uint32_t now) {
  static uint32_t lastPoll = 0;
  if (now - lastPoll < 16) return;
  lastPoll = now;

  int16_t x, y;
  bool down = touch.getPoint(&x, &y, 1) > 0;
#if IMAGE_FIELDS
  // Cualquier contacto detiene la respiracion y reinicia la espera: mientras
  // haya una mano encima, el campo se queda donde el pulgar lo dejo.
  if (down) lastInteract = now;
#endif

  if (down && !touchDown) {
    gx0 = gxl = x;
    gy0 = gyl = y;
    gStart = now;
    gAxis = 0;
    gLongFired = false;
    gDepth0 = fs.depth;
    if (screenOff) { screenOff = false; boardBrightness(storeSettings().bright); }
  } else if (down) {
    gxl = x;
    gyl = y;
    int dx = gxl - gx0, dy = gyl - gy0;

    if (mode == M_FIELD) {
      if (gAxis == 0 && (abs(dx) > 12 || abs(dy) > 12)) gAxis = (abs(dy) > abs(dx)) ? 1 : 2;

      if (gAxis == 1) {
        // arrastrar hacia arriba reduce mas: el gesto "sube" hacia lo evocativo
        float d = gDepth0 - (float)dy / 200.0f;
        fs.depth = d < 0 ? 0 : (d > 1 ? 1 : d);
        showDepthBig = true;
        hudUntil = now + 2500;
      } else if (gAxis == 0 && !gLongFired && now - gStart > 900 &&
                 abs(dx) < 12 && abs(dy) < 12) {
        gLongFired = true;
        openKeyboard();
      }
    }
  } else if (touchDown) {
    int dx = gxl - gx0, dy = gyl - gy0;
    uint32_t dt = now - gStart;
    bool tap = (dt < 500 && abs(dx) < 14 && abs(dy) < 14);

    // Esquina izquierda: en el campo abre la primera pantalla, en las demas
    // vuelve al campo (y cancela el teclado). Esquina derecha: sigue el ciclo.
    // Con solo la izquierda, BAND / SIX FIELDS / SETTINGS eran inalcanzables.
    if (tap && !gLongFired && inCorner(gxl, gyl)) {
      if (mode == M_FIELD) {
        nextMode();
      } else {
        mode = M_FIELD;
        hudUntil = now + 2000;
        buzzPing(660, 30);
      }
      showDepthBig = false;
      touchDown = down;
      return;
    }
    if (tap && mode != M_FIELD && mode != M_KEY && inNextCorner(gxl, gyl)) {
      nextMode();
      touchDown = down;
      return;
    }

    if (mode == M_FIELD) {
      if (gAxis == 2 && abs(dx) > 55) {
        // cambiar de campo: cada uno recuerda su propia profundidad
        depthByAtmos[fs.atmos] = fs.depth;
        int n = (int)fs.atmos + (dx < 0 ? 1 : -1);
        if (n < 0) n = ATMOS_COUNT - 1;
        if (n >= ATMOS_COUNT) n = 0;
        fs.atmos = (uint8_t)n;
        fs.depth = depthByAtmos[fs.atmos];
        hudUntil = now + 2200;
        buzzPing(ATMOS[fs.atmos].voicePitch, 70);
      } else if (tap && !gLongFired) {
        hudUntil = (now < hudUntil) ? 0 : now + 3000;
      }
      showDepthBig = false;
      if (gAxis == 1) {
        depthByAtmos[fs.atmos] = fs.depth;
        storeSettings().lastDepth = (uint8_t)(fs.depth * 255);
        storeSaveSettings();
      }
    } else if (mode == M_KEY && tap) {
      keyboardTap(gxl, gyl);
    } else if (mode == M_SET && tap) {
      settingsTap(gxl, gyl);
    } else if (mode == M_INDEX && tap) {
      int c = (gxl - IDX_X) / (IDX_W + IDX_GAPX), r = (gyl - IDX_Y) / (IDX_H + IDX_GAPY);
      int i = r * IDX_COLS + c;
      if (c >= 0 && c < IDX_COLS && r >= 0 && r < IDX_ROWS && i < ATMOS_COUNT) {
        depthByAtmos[fs.atmos] = fs.depth;
        fs.atmos = (uint8_t)i;
        fs.depth = depthByAtmos[i];
        mode = M_FIELD;
        hudUntil = now + 2500;
        buzzPing(ATMOS[i].voicePitch, 70);
      }
    } else if ((mode == M_NAMES || mode == M_BAND) && tap) {
      mode = M_FIELD;
      hudUntil = now + 2000;
    }
  }
  touchDown = down;
}

// Apagado real: el AXP2101 corta la alimentacion. Consumo cero, no "pantalla
// apagada gastando 45 mA". Se guarda antes, porque despues no hay despues.
static void powerOff() {
  storeSettings().lastAtmos = fs.atmos;
  storeSettings().lastDepth = (uint8_t)(fs.depth * 255);
  storeSaveSettings();
  storeFlush();
  buzzSilence();
  boardBrightness(0);
  delay(60);
  boardPowerOff();          // el AXP2101 corta de verdad: consumo cero
  delay(2000);              // con USB conectado no se apaga: se sigue como si nada
}

// En la 1.69 habia que aprender el nivel de reposo del pulsador porque el
// enclavamiento no estaba documentado. Aqui el PMIC lo resuelve y esto es una
// linea.
static bool btnPressed() { return boardButtonDown(); }

static void handleButton(uint32_t now) {
  static bool was = false;
  static uint32_t downAt = 0;
  static bool longFired = false;
  bool d = btnPressed();

  if (d && !was) {
    downAt = now;
    longFired = false;
  } else if (d && !longFired && now - downAt > 1400) {
    longFired = true;
    buzzPing(320, 140);
    powerOff();
  } else if (!d && was && !longFired) {
    uint32_t held = now - downAt;
    if (held > 40) nextMode();
  }
  was = d;
}

// ---------------------------------------------------------------------------
// sensores
// ---------------------------------------------------------------------------

static void updateAgitation(uint32_t now) {
  static uint32_t last = 0;
  static float prevMag = 1.0f;
  if (!imuOk || now - last < 60) return;
  last = now;
  float ax, ay, az;
  if (!imu.getAccelerometer(ax, ay, az)) return;
  float mag = sqrtf(ax * ax + ay * ay + az * az);
  float jerk = fabsf(mag - prevMag);
  prevMag = mag;
  // Quieto, el campo se asienta. Al moverlo, se altera. Es la unica via por la
  // que el cuerpo entra en el aparato, y la que hace que sostenerlo quieto valga.
  float target = jerk * 3.4f;
  if (target > 1.0f) target = 1.0f;
  fs.agitation += (target - fs.agitation) * (target > fs.agitation ? 0.35f : 0.02f);

  // Sostenido por alguien, el acelerometro nunca se queda del todo quieto.
  // Sobre la mesa, si. Es una diferencia pequena y muy fiable, y decide si los
  // pulsos pueden permitirse ser largos: si no hay nadie, no hay a quien
  // acompanar.
  static float micro = 0.0f;
  micro += (jerk - micro) * 0.05f;
  (void)micro;
}

static void updateDaylight(uint32_t now) {
  static uint32_t last = 0;
  if (!rtcOk || (last && now - last < 60000)) return;
  last = now ? now : 1;
  RTC_DateTime t = rtc.getDateTime();
  int h = t.getHour();
  setHour = h;
  setMin = t.getMinute();
  // curva simple: mas oscuro de madrugada, mas claro a mediodia
  float x = (h + t.getMinute() / 60.0f - 4.0f) / 24.0f;
  fs.daylight = 0.5f - 0.5f * cosf(x * 2.0f * (float)PI);
}

// ---------------------------------------------------------------------------
// consola USB: volcado de datos
// ---------------------------------------------------------------------------

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line == "DUMP") {
    Serial.println("field,name,depth,minute_of_day");
    for (int a = 0; a < ATMOS_COUNT; a++)
      for (uint8_t i = 0; i < storeNameCount(a); i++) {
        const NameRec &r = storeName(a, i);
        Serial.printf("%s,%s,%.3f,%lu\n", ATMOS_NUMERAL[a], r.word,
                      r.depth / 255.0f, (unsigned long)(r.epoch / 60));
      }
    Serial.println("END");
  } else if (line == "BAND") {
    Serial.println("bucket,depth,seconds");
    const uint32_t *b = storeBand();
    for (int i = 0; i < BAND_BUCKETS; i++)
      Serial.printf("%d,%.3f,%lu\n", i, (float)i / (BAND_BUCKETS - 1), (unsigned long)b[i]);
    Serial.printf("mean,%.3f,%lu\n", storeBandMean(), (unsigned long)storeBandTotal());
    Serial.println("END");
  } else if (line == "SOURCES") {
    // deliberadamente NO esta en la pantalla: pedirlo es una decision consciente
    for (int a = 0; a < ATMOS_COUNT; a++)
      Serial.printf("%s,%s\n", ATMOS_NUMERAL[a], ATMOS[a].source);
    Serial.println("END");
  } else if (line == "TEST") {
    Serial.println("-- speaker --");
    buzzSelfTest([](const char *m) { Serial.println(m); });
    Serial.println("END");
#if IMAGE_FIELDS
  } else if (line == "TIME") {
    // Los milisegundos reales de la placa, por etapa y por nivel. Es la unica
    // forma honesta de afinar esto: aqui no se adivina el reloj de nadie.
    for (int lvl = 0; lvl < 2; lvl++) {
      int w = lvl ? RPC_CW : RPC_W;
      Serial.printf("-- %d x %d --\n", w, w);
      for (int k = 0; k < 3; k++) {
        float d = 0.30f + 0.225f * k;
        rpcForget();                       // en frio: medir la cache no dice nada
        uint32_t t0 = millis();
        if (lvl) rpcDrag(gfx->getFramebuffer(), fs.atmos, d);
        else     rpcStill(gfx->getFramebuffer(), fs.atmos, d);
        uint32_t wall = millis() - t0;
        const RpcTiming &t = rpcTiming();
        Serial.printf("d=%.2f Q=%2d  blur %3u  keys %3u  sort %3u  scatter %3u"
                      "  reduce %3u  con volcado %3u ms\n",
                      d, rpcQForDepth(d), t.blur, t.keys, t.sort, t.scatter,
                      t.total, wall);
      }
    }
    Serial.printf("respiracion: %d de %d estados listos\n",
                  rpcBreathReady(), rpcBreathTotal());
    Serial.printf("PSRAM libre %u de %u bytes\n",
                  (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
    Serial.println("END");
#endif
  } else if (line == "RESET") {
    for (int a = 0; a < ATMOS_COUNT; a++) storeClearNames(a);
    Serial.println("OK names cleared");
  }
}

// ---------------------------------------------------------------------------

void setup() {
  // LO PRIMERO, SIEMPRE. Esta placa vuelve a tener enclavamiento por firmware:
  // sin esto se apaga en cuanto se suelta el boton, que es exactamente lo que
  // dejo muerta la 1.69.
  boardLatchPower();

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  ledcAttach(LCD_BL, 5000, 8);
  boardBrightness(0);   // se sube tras el primer fotograma: sin destello blanco

  I2C_Init();
  TCA9554PWR_Init(0x00);          // los ocho EXIO como salidas
  Set_EXIO(EXIO_LCD_RST, Low);    // reset del panel: cuelga del expansor
  delay(50);
  Set_EXIO(EXIO_LCD_RST, High);
  delay(120);

  if (!gfx->begin(40000000)) Serial.println("canvas/panel begin failed");
  gfx->fillScreen(0);
  gfx->flush();

  Touch_Init();

  storeBegin();
  buzzBegin();
  buzzSetEnabled(storeSettings().sound);

  imuOk = imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
  if (imuOk) {
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_62_5Hz,
                            SensorQMI8658::LPF_MODE_3);
    imu.enableAccelerometer();
  } else {
    Serial.println("QMI8658 not found");
  }

  rtcOk = rtc.begin(Wire, I2C_SDA_PIN, I2C_SCL_PIN);
  if (!rtcOk) Serial.println("PCF85063 not found");

  for (int i = 0; i < ATMOS_COUNT; i++) depthByAtmos[i] = storeSettings().lastDepth / 255.0f;
  fs.atmos = storeSettings().lastAtmos;
  fs.depth = depthByAtmos[fs.atmos];

  updateDaylight(millis());
  lastDwell = millis();
  hudUntil = millis() + 3500;
  Serial.printf("Pocket Atmosphere v%s\n", FW_VERSION);
}

void loop() {
  uint32_t now = millis();

  handleTouch(now);
  handleButton(now);
  handleSerial();
  updateAgitation(now);
  updateDaylight(now);

  const Atmos &a = ATMOS[fs.atmos];
  buzzUpdate(now, a.voicePitch, a.voiceSpread, a.voiceRate, fs.agitation, fs.depth);

  if (screenOff) {
    delay(30);
    return;
  }

  if (now - lastFrame < 42) return;  // ~24 fps: el volcado SPI es el techo
  float dt = (now - lastFrame) / 1000.0f;
  lastFrame = now;

  // la deriva se acelera con la agitacion; el campo tarda en calmarse otra vez
  fs.t += dt * (1000.0f / a.driftMs) * (0.6f + 1.8f * fs.agitation);

  // el tiempo en cada profundidad solo cuenta mirando el campo, no los menus
  if (mode == M_FIELD) storeDwell(fs.depth, now - lastDwell);
  lastDwell = now;

  switch (mode) {
    case M_FIELD: renderField(now); break;
    case M_NAMES: renderNames(); break;
    case M_BAND: renderBand(); break;
    case M_INDEX: renderIndex(); break;
    case M_SET: renderSettings(); break;
    case M_KEY: renderKeyboard(); break;
  }
  gfx->flush();

  static bool lit = false;
  if (!lit) { lit = true; boardBrightness(storeSettings().bright); }
}
