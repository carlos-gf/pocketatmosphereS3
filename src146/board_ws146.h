#pragma once
// ---------------------------------------------------------------------------
// CAPA DE PLACA: Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 redonda, SPD2010)
//
// Expone los mismos nombres que usaban las versiones anteriores para que el
// resto del programa no se entere de en que aparato esta corriendo.
//
// Diferencias que importan respecto a la CoreS3:
//
//  * La pantalla es REDONDA. 412x412, pero solo el disco inscrito existe. El
//    cuadrado util interior mide 291 px de lado; todo lo que sea texto o
//    control vive ahi dentro.
//  * El reset del panel NO es un GPIO: cuelga del expansor TCA9554 (EXIO2).
//  * VUELVE EL ENCLAVAMIENTO DE ALIMENTACION. GPIO7 hay que subirlo lo PRIMERO
//    o la placa se apaga al soltar el boton. Ese olvido mato la 1.69 y costo
//    una semana; aqui es la primera linea de setup().
//  * El altavoz y el microfono estan en buses I2S SEPARADOS, asi que -al
//    contrario que en la CoreS3- pueden convivir. Grabar ambientes deja de ser
//    excluyente con sonar.
//  * El framebuffer de 412x412 son 340 kB: no cabe en la RAM interna del S3.
//    Va a PSRAM, y por eso el lienzo esta subclasificado.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Touch_SPD2010.h"

#define LCD_WIDTH  412
#define LCD_HEIGHT 412
#define LCD_CX     206
#define LCD_CY     206
#define LCD_R      206
// radio seguro para texto y controles: dentro de este circulo nada se sale
#define LCD_SAFE_R 176

// ---- pines (de los ejemplos oficiales de Waveshare) -------------------------
#define QSPI_CS    21
#define QSPI_SCK   40
#define QSPI_D0    46
#define QSPI_D1    45
#define QSPI_D2    42
#define QSPI_D3    41
#define LCD_TE     18
#define LCD_BL      5

#define PWR_KEY_PIN     6    // pulsador lateral, activo a nivel bajo
#define PWR_LATCH_PIN   7    // <-- subirlo lo primero de todo
#define BAT_ADC_PIN     8

#define I2S_SPK_DOUT   47
#define I2S_SPK_BCLK   48
#define I2S_SPK_LRCK   38

#define EXIO_LCD_RST   EXIO_PIN2

// ---- lienzo en PSRAM --------------------------------------------------------
class PACanvas : public Arduino_Canvas {
public:
  PACanvas(Arduino_G *out) : Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, out) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    if (!_framebuffer) {
      _framebuffer = (uint16_t *)ps_malloc((size_t)LCD_WIDTH * LCD_HEIGHT * 2);
      if (!_framebuffer) return false;
    }
    return Arduino_Canvas::begin(speed);
  }
};

// ---- tactil -----------------------------------------------------------------
// El SPD2010 hace pantalla y tactil en el mismo chip. Se conserva la forma de
// la version anterior para no tocar la logica de gestos.
struct TouchShim {
  int getPoint(int16_t *x, int16_t *y, uint8_t) {
    Touch_Loop();
    if (touch_data.touch_num == 0) return 0;
    *x = (int16_t)touch_data.rpt[0].x;
    *y = (int16_t)touch_data.rpt[0].y;
    return 1;
  }
};

// ---- alimentacion, brillo, bateria -----------------------------------------
static inline void boardBrightness(uint8_t v) { ledcWrite(LCD_BL, v); }

static inline int boardBatteryPct() {
  uint32_t mv = 0;
  for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
  float v = (mv / 4.0f) * 3.0f / 1000.0f;   // divisor 1/3 en la placa
  int pct = (int)((v - 3.30f) / 0.90f * 100.0f);
  return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// No hay chip de carga que lo diga por I2C: se deduce de la tension, que sube
// por encima de lo que da la celda sola cuando el USB esta alimentando.
static inline bool boardCharging() {
  uint32_t mv = analogReadMilliVolts(BAT_ADC_PIN);
  return (mv * 3.0f / 1000.0f) > 4.25f;
}

static inline void boardPowerOff() { digitalWrite(PWR_LATCH_PIN, LOW); }
static inline bool boardButtonDown() { return digitalRead(PWR_KEY_PIN) == LOW; }

// Lo PRIMERO de setup(). Sin esto la placa vive solo mientras se aprieta PWR.
static inline void boardLatchPower() {
  pinMode(PWR_LATCH_PIN, OUTPUT);
  digitalWrite(PWR_LATCH_PIN, HIGH);
  pinMode(PWR_KEY_PIN, INPUT);
}
