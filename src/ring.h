#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// ANILLO SK6812 en PORT.B.
//
// La luz NO es un indicador. Es el mismo campo escapandose por el borde: toma
// el color medio de lo que hay en pantalla, con el brillo limitado, y responde
// a los mismos acontecimientos que la voz y el motor.
//
// El limite de brillo no es prudencia, es diseno y tambien bateria: doce LED a
// pleno blanco piden cerca de un amperio, que ni el bus de 5 V del Grove da ni
// la celda aguanta. A la intensidad a la que un resplandor ambiente funciona de
// verdad -entre el 10 y el 15%- son 100..150 mA.
// ---------------------------------------------------------------------------

void ringBegin(uint8_t count);
// vuelve a montar el anillo en otro pin, en caliente (para probar por USB)
void ringRewire(uint8_t pin, uint8_t count);
// enciende todo en blanco al tope permitido durante ms: prueba de cableado
void ringSelfTest(uint16_t ms);
uint8_t ringPin();
void ringSetEnabled(bool on);
// color de fondo del anillo (RGB565, tal cual sale del campo) y brillo 0..1
void ringSetField(uint16_t rgb565, float brightness);
// acontecimiento: un realce breve sobre el fondo
void ringPulse(float level);
void ringUpdate(uint32_t now);
void ringOff();
