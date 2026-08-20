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
#include "board_cores3.h"
#include "atmos.h"
#include "field.h"
#include "store.h"
#include "buzz.h"
#include "haptic.h"
#include "ring.h"

#define FW_VERSION "0.1-s3"

static PACanvas canvas;
static PACanvas *gfx = &canvas;

static TouchShim touch;
static ImuShim imu;
static RtcShim rtc;
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

// La 1.69 tenia un pulsador lateral para pasar de pantalla. La CoreS3 solo
// tiene el de encendido, asi que la navegacion vuelve a la pantalla: una
// esquina viva arriba a la izquierda. En el campo son tres rayas (ir al
// siguiente cuadro); en los demas cuadros es una flecha (volver al campo).
#define CORNER 44

static void drawCorner(uint16_t col, bool back) {
  if (back) {
    // flecha a la izquierda
    for (int i = 0; i < 8; i++) gfx->drawFastHLine(14 + i, 20 - i, 1, col);
    for (int i = 0; i < 8; i++) gfx->drawFastHLine(14 + i, 20 + i, 1, col);
    gfx->drawFastHLine(14, 20, 18, col);
  } else {
    for (int i = 0; i < 3; i++) gfx->fillRect(14, 14 + i * 6, 18, 2, col);
  }
}

static bool inCorner(int x, int y) { return x < CORNER && y < CORNER; }

static void drawBattery(int x, int y, uint16_t col) {
  int pct = boardBatteryPct();   // el AXP2101 ya lo sabe: sin divisor ni ADC
  gfx->drawRect(x, y, 18, 9, col);
  gfx->fillRect(x + 18, y + 3, 2, 3, col);
  gfx->fillRect(x + 2, y + 2, (16 * pct) / 100, 5, col);
}

// ---------------------------------------------------------------------------
// pantalla principal: el campo
// ---------------------------------------------------------------------------

static void renderField(uint32_t now) {
  fieldRender(gfx->getFramebuffer(), fs);

  bool hud = (now < hudUntil) || showDepthBig;
  uint16_t ink = inkOn(fs);
  // La esquina se dibuja SIEMPRE, tambien con la interfaz retirada. Es la unica
  // salida que hay, y una salida que desaparece no es una salida.
  drawCorner(ink, false);

  if (revealUntil && now < revealUntil) {
    // tras nombrar, si el ajuste esta activo, se dice de donde venia
    const char *src = ATMOS[fs.atmos].source;
    gfx->fillRect(0, 96, LCD_WIDTH, 44, groundOf(fs));
    textMid(LCD_WIDTH / 2, 104, "it was made from", inkOfGround(fs), 1);
    textMid(LCD_WIDTH / 2, 120, src, inkOfGround(fs), 1);
  } else if (revealUntil && now >= revealUntil) {
    revealUntil = 0;
  }

  if (!hud) return;

  textAt(10, 10, ATMOS_NUMERAL[fs.atmos], ink, 2);
  drawBattery(LCD_WIDTH - 30, 12, ink);

  // barra de profundidad: linea fina abajo, llena hasta el valor actual
  int by = LCD_HEIGHT - 24, bx = 20, bw = LCD_WIDTH - 40;
  gfx->drawFastHLine(bx, by, bw, ink);
  int px = bx + (int)(bw * fs.depth);
  gfx->fillRect(px - 1, by - 5, 3, 11, ink);

  char lab[24];
  snprintf(lab, sizeof lab, "reduction %.2f", fs.depth);
  textMid(LCD_WIDTH / 2, by + 10, lab, ink, 1);

  if (showDepthBig) {
    snprintf(lab, sizeof lab, "%.2f", fs.depth);
    textMid(LCD_WIDTH / 2, 96, lab, ink, 5);
    snprintf(lab, sizeof lab, "%d colours", fieldLevels(fs.depth));
    textMid(LCD_WIDTH / 2, 144, lab, ink, 1);
  } else {
    uint8_t n = storeNameCount(fs.atmos);
    if (n) {
      snprintf(lab, sizeof lab, "%u names", (unsigned)n);
      textMid(LCD_WIDTH / 2, 34, lab, ink, 1);
    } else {
      textMid(LCD_WIDTH / 2, 34, "hold to name it", ink, 1);
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
  textMid(LCD_WIDTH / 2, 16, h, ink, 2);
  drawCorner(ink, true);

  uint8_t n = storeNameCount(fs.atmos);
  if (n == 0) {
    textMid(LCD_WIDTH / 2, 110, "no one has named", ink, 1);
    textMid(LCD_WIDTH / 2, 126, "this one yet", ink, 1);
  } else {
    // el eje de la derecha es la profundidad a la que se dijo cada palabra
    textAt(150, 38, "light", ink, 1);
    textAt(198, 38, "deep", ink, 1);
    // Cada fila: la palabra, y a que profundidad se dijo. Ver la lista completa
    // ES el punto: la divergencia deja de ser un numero y se vuelve legible.
    for (uint8_t i = 0; i < n && i < 10; i++) {
      const NameRec &r = storeName(fs.atmos, n - 1 - i);
      int y = 54 + i * 20;
      textAt(16, y, r.word, ink, 1);
      int bx = 150, bw = 74;
      gfx->drawFastHLine(bx, y + 4, bw, bg == 0 ? ink : ink);
      int px = bx + (r.depth * bw) / 255;
      gfx->fillRect(px - 1, y, 3, 9, ink);
    }
  }
  textMid(LCD_WIDTH / 2, LCD_HEIGHT - 18, "corner: back to the field", ink, 1);
}

// ---------------------------------------------------------------------------
// pantalla: tu banda
// ---------------------------------------------------------------------------

static void renderBand() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_WIDTH / 2, 16, "YOUR BAND", ink, 2);
  drawCorner(ink, true);
  textMid(LCD_WIDTH / 2, 40, "where you settle", ink, 1);

  const uint32_t *b = storeBand();
  uint32_t peak = 1;
  for (int i = 0; i < BAND_BUCKETS; i++)
    if (b[i] > peak) peak = b[i];

  int x0 = 16, w = LCD_WIDTH - 32, base = 176, hmax = 104;
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
    textMid(LCD_WIDTH / 2, base + 22, "not enough time yet", ink, 1);
  } else {
    int px = x0 + (int)(m * w);
    gfx->fillRect(px - 1, base - hmax - 8, 3, 8, ink);
    snprintf(s, sizeof s, "mean %.2f", m);
    textMid(LCD_WIDTH / 2, base + 18, s, ink, 1);
    snprintf(s, sizeof s, "%lum %lus held", (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    textMid(LCD_WIDTH / 2, base + 32, s, ink, 1);
  }
  textMid(LCD_WIDTH / 2, LCD_HEIGHT - 18, "corner: back to the field", ink, 1);
}

// ---------------------------------------------------------------------------
// pantalla: la coleccion
// ---------------------------------------------------------------------------

// 320x240 es apaisada: la rejilla gira a 3 x 2 y las muestras se ensanchan.
#define IDX_COLS 3
#define IDX_ROWS 2
#define IDX_W 92
#define IDX_H 66
#define IDX_GAPX 10
#define IDX_GAPY 26
#define IDX_X ((LCD_WIDTH - IDX_COLS * IDX_W - (IDX_COLS - 1) * IDX_GAPX) / 2)
#define IDX_Y 44

static void renderIndex() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_WIDTH / 2, 16, "SIX FIELDS", ink, 2);
  drawCorner(ink, true);

  for (int i = 0; i < ATMOS_COUNT; i++) {
    int c = i % IDX_COLS, r = i / IDX_COLS;
    int x = IDX_X + c * (IDX_W + IDX_GAPX), y = IDX_Y + r * (IDX_H + IDX_GAPY);
    FieldState s = fs;
    s.atmos = i;
    s.depth = depthByAtmos[i];
    s.t = fs.t * 0.6f + i * 7.3f;
    fieldSwatch(gfx->getFramebuffer(), LCD_WIDTH, LCD_HEIGHT, x, y, IDX_W, IDX_H, s);
    if (i == fs.atmos) gfx->drawRect(x - 2, y - 2, IDX_W + 4, IDX_H + 4, ink);
    char lab[16];
    snprintf(lab, sizeof lab, "%s  %u", ATMOS_NUMERAL[i], (unsigned)storeNameCount(i));
    textAt(x, y + IDX_H + 4, lab, ink, 1);
  }
  textMid(LCD_WIDTH / 2, LCD_HEIGHT - 10, "tap one", ink, 1);
}

// ---------------------------------------------------------------------------
// pantalla: ajustes
// ---------------------------------------------------------------------------

struct Row { int y; const char *label; };
static const Row SET_ROWS[] = {
  { 48, "hour" }, { 76, "minute" }, { 104, "sound" }, { 132, "reveal source" }, { 160, "brightness" },
};
#define SET_N 5
static int setHour = 12, setMin = 0;

static void renderSettings() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_WIDTH / 2, 16, "SETTINGS", ink, 2);
  drawCorner(ink, true);

  Settings &st = storeSettings();
  char v[24];
  for (int i = 0; i < SET_N; i++) {
    textAt(16, SET_ROWS[i].y, SET_ROWS[i].label, ink, 1);
    switch (i) {
      case 0: snprintf(v, sizeof v, "%02d", setHour); break;
      case 1: snprintf(v, sizeof v, "%02d", setMin); break;
      case 2: snprintf(v, sizeof v, "%s", st.sound ? "on" : "off"); break;
      case 3: snprintf(v, sizeof v, "%s", st.reveal ? "on" : "off"); break;
      default: snprintf(v, sizeof v, "%u", (unsigned)st.bright); break;
    }
    textAt(200, SET_ROWS[i].y, v, ink, 1);
    gfx->drawRect(170, SET_ROWS[i].y - 6, 20, 20, ink);
    textAt(176, SET_ROWS[i].y, "-", ink, 1);
    gfx->drawRect(246, SET_ROWS[i].y - 6, 20, 20, ink);
    textAt(252, SET_ROWS[i].y, "+", ink, 1);
  }
  textMid(LCD_WIDTH / 2, 196, "USB: type DUMP for data", ink, 1);
  char fw[32];
  snprintf(fw, sizeof fw, "Pocket Atmosphere v%s", FW_VERSION);
  textMid(LCD_WIDTH / 2, 216, fw, ink, 1);
}

static void settingsTap(int16_t x, int16_t y) {
  Settings &st = storeSettings();
  for (int i = 0; i < SET_N; i++) {
    if (y < SET_ROWS[i].y - 8 || y > SET_ROWS[i].y + 16) continue;
    int dir = (x >= 190) ? +1 : (x <= 146 && x >= 114) ? -1 : 0;
    if (!dir) return;
    switch (i) {
      case 0: setHour = (setHour + dir + 24) % 24; break;
      case 1: setMin = (setMin + dir + 60) % 60; break;
      case 2: st.sound = !st.sound; buzzSetEnabled(st.sound); hapticSetEnabled(st.sound); break;
      case 3: st.reveal = !st.reveal; break;
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
#define KB_X 20
#define KB_Y 96
#define KB_KW 40
#define KB_KH 26

static void renderKeyboard() {
  uint16_t bg = groundOf(fs), ink = inkOfGround(fs);
  gfx->fillScreen(bg);
  textMid(LCD_WIDTH / 2, 18, "WHAT IS IT?", ink, 2);
  drawCorner(ink, true);   // salir sin nombrar: no todo el mundo quiere hacerlo
  textMid(LCD_WIDTH / 2, 42, "your word, not ours", ink, 1);

  gfx->drawRect(20, 64, LCD_WIDTH - 40, 30, ink);
  textAt(28, 74, nameBuf, ink, 2);

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
  gfx->drawRect(KB_X, y, 100, 26, ink);
  textAt(KB_X + 34, y + 9, "DEL", ink, 1);
  gfx->drawRect(KB_X + 112, y, 100, 26, ink);
  textAt(KB_X + 146, y + 9, "OK", ink, 1);
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
  int by = KB_Y + 4 * KB_KH;
  if (y >= by && y <= by + 26) {
    if (x < KB_X + 100) {
      if (nameLen) nameBuf[--nameLen] = 0;
      buzzPing(600, 22);
    } else if (x >= KB_X + 112) {
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
  hapticStop();
  ringOff();
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
  } else if (line == "RESET") {
    for (int a = 0; a < ATMOS_COUNT; a++) storeClearNames(a);
    Serial.println("OK names cleared");
  }
}

// ---------------------------------------------------------------------------

void setup() {
  // M5Unified arranca el PMIC, la pantalla, el tactil, el IMU, el RTC y el
  // altavoz de una vez. Ya no hay que enclavar la alimentacion a mano: lo hace
  // el AXP2101. Ese olvido fue el fallo que dejo la version anterior muerta en
  // cuanto se soltaba el boton.
  auto cfg = M5.config();
  cfg.clear_display = true;
  cfg.output_power  = true;    // 5 V en los Grove: hace falta para el anillo RGB
  cfg.internal_imu  = true;
  cfg.internal_rtc  = true;
  cfg.internal_spk  = true;
  // El micro y el altavoz comparten el bus I2S en la CoreS3: dejar los dos
  // encendidos convierte el sonido en estatica. Cuando llegue el momento de
  // grabar ambientes habra que apagar el altavoz mientras se graba, no tener
  // ambos a la vez.
  cfg.internal_mic  = false;
  M5.begin(cfg);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);

  boardBrightness(0);  // se sube tras el primer fotograma: sin destello blanco

  if (!gfx->begin()) Serial.println("canvas alloc failed");
  gfx->fillScreen(0);
  gfx->flush();

  storeBegin();
  buzzBegin();
  buzzSetEnabled(storeSettings().sound);
  hapticBegin();
  hapticSetEnabled(storeSettings().sound);
  ringBegin(12);              // cambia el numero si tu anillo tiene otro

  imuOk = M5.Imu.isEnabled();
  if (!imuOk) Serial.println("IMU not found");
  rtcOk = M5.Rtc.isEnabled();
  if (!rtcOk) Serial.println("RTC not found");

  for (int i = 0; i < ATMOS_COUNT; i++) depthByAtmos[i] = storeSettings().lastDepth / 255.0f;
  fs.atmos = storeSettings().lastAtmos;
  fs.depth = depthByAtmos[fs.atmos];

  updateDaylight(millis());
  lastDwell = millis();
  hudUntil = millis() + 3500;
  Serial.printf("Pocket Atmosphere v%s\n", FW_VERSION);
}

void loop() {
  M5.update();   // sondea botones, tactil, PMIC
  uint32_t now = millis();

  handleTouch(now);
  handleButton(now);
  handleSerial();
  updateAgitation(now);
  updateDaylight(now);

  const Atmos &a = ATMOS[fs.atmos];
  buzzUpdate(now, a.voicePitch, a.voiceSpread, a.voiceRate, fs.agitation, fs.depth);
  hapticUpdate(now);
  ringSetField(fieldMidColor(fs), 0.35f + 0.5f * (1.0f - fs.depth));
  ringUpdate(now);

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
