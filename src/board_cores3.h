#pragma once
// ---------------------------------------------------------------------------
// CAPA DE PLACA: M5Stack CoreS3 (ESP32-S3, 320x240 IPS tactil)
//
// El aparato nacio sobre una Waveshare 1.69, donde cada pieza -pantalla, tactil,
// IMU, RTC, zumbador, enclavamiento de alimentacion- era una libreria distinta y
// un pin distinto. En la CoreS3 todo eso lo da M5Unified, asi que este fichero
// existe para que el resto del programa NO se entere del cambio: expone los
// mismos nombres que usaba antes y por dentro llama a M5.
//
// Lo que desaparece respecto a la 1.69:
//   - Arduino_GFX + Arduino_ST7789   -> M5.Display / M5Canvas
//   - SensorLib (CST816, QMI8658, PCF85063) -> M5.Touch / M5.Imu / M5.Rtc
//   - el enclavamiento SYS_EN/SYS_OUT -> M5.Power (lo hace el AXP2101)
//   - el divisor resistivo de bateria -> M5.Power.getBatteryLevel()
//   - el zumbador piezo por LEDC      -> M5.Speaker (I2S, altavoz de verdad)
// ---------------------------------------------------------------------------

#include <M5Unified.h>

#define LCD_WIDTH  320
#define LCD_HEIGHT 240

// ---- lienzo -----------------------------------------------------------------
// El campo se dibuja entero en RAM y se vuelca de una vez: sin esto se ve
// rasgado, porque el renderizador escribe el fotograma pixel a pixel.
class PACanvas : public M5Canvas {
public:
  PACanvas() : M5Canvas(&M5.Display) {}
  bool begin(uint32_t = 0) {
    setColorDepth(16);
    setPsram(true);
    return createSprite(LCD_WIDTH, LCD_HEIGHT) != nullptr;
  }
  uint16_t *getFramebuffer() { return (uint16_t *)getBuffer(); }
  void flush() { pushSprite(0, 0); }
};

// ---- tactil -----------------------------------------------------------------
struct TouchShim {
  int getPoint(int16_t *x, int16_t *y, uint8_t) {
    if (M5.Touch.getCount() == 0) return 0;
    auto d = M5.Touch.getDetail(0);
    *x = d.x;
    *y = d.y;
    return 1;
  }
};

// ---- IMU --------------------------------------------------------------------
struct ImuShim {
  bool getAccelerometer(float &ax, float &ay, float &az) {
    return M5.Imu.getAccel(&ax, &ay, &az);
  }
};

// ---- RTC --------------------------------------------------------------------
// Se conserva la forma de SensorPCF85063 para no tocar las llamadas de arriba.
struct RTC_DateTime {
  int y, mo, d, h, mi, s;
  RTC_DateTime() : y(2026), mo(1), d(1), h(12), mi(0), s(0) {}
  RTC_DateTime(int Y, int Mo, int D, int H, int Mi, int S)
      : y(Y), mo(Mo), d(D), h(H), mi(Mi), s(S) {}
  int getHour() const { return h; }
  int getMinute() const { return mi; }
};

struct RtcShim {
  RTC_DateTime getDateTime() {
    auto t = M5.Rtc.getDateTime();
    return RTC_DateTime(t.date.year, t.date.month, t.date.date,
                        t.time.hours, t.time.minutes, t.time.seconds);
  }
  void setDateTime(const RTC_DateTime &v) {
    m5::rtc_datetime_t t;
    t.date.year = v.y; t.date.month = v.mo; t.date.date = v.d; t.date.weekDay = 0;
    t.time.hours = v.h; t.time.minutes = v.mi; t.time.seconds = v.s;
    M5.Rtc.setDateTime(t);
  }
};

// ---- alimentacion, brillo, bateria -----------------------------------------
static inline void boardBrightness(uint8_t v) { M5.Display.setBrightness(v); }
static inline int boardBatteryPct() {
  int p = M5.Power.getBatteryLevel();
  return p < 0 ? 0 : (p > 100 ? 100 : p);
}
static inline void boardPowerOff() { M5.Power.powerOff(); }
// El boton lateral izquierdo. En la 1.69 habia que aprender el nivel de reposo
// de SYS_OUT a mano; aqui el PMIC ya lo resuelve.
static inline bool boardButtonDown() { return M5.BtnPWR.isPressed(); }

// ---- expansion (Grove) ------------------------------------------------------
// PORT.B y PORT.C son GPIO libres con 5 V y masa. Ahi van el anillo RGB y el
// motor de vibracion, sin soldar nada.
//
// El Unit de vibracion (akita11 LightVibratorUnit, compatible con el oficial de
// M5Stack) lleva la senal en el PIN 2 del conector Grove; el pin 1 no esta
// conectado. Funciona igual a 5 V que a 3,3 V.
//
// Cual de los dos GPIO de un puerto es "pin 1" y cual "pin 2" no lo dice la
// documentacion de forma inequivoca. Si el motor no se mueve, cambiar
// HAPTIC_PIN por el otro numero del puerto y volver a compilar: es lo unico
// que hay que tocar.
#define PORT_B_PIN1 8
#define PORT_B_PIN2 9
#define PORT_C_PIN1 17
#define PORT_C_PIN2 18

#define LED_RING_PIN PORT_B_PIN1   // anillo SK6812 (datos), PORT.B
#define HAPTIC_PIN   PORT_C_PIN1   // motor de vibracion (PWM), PORT.C
