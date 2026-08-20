#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// El motor de vibracion como TERCER CANAL del mismo acontecimiento.
//
// No es un aviso ni una confirmacion: cuando el campo tiene un evento, suena y
// se siente a la vez, con la misma envolvente. Lo audible y lo tactil son la
// misma curva a distinta frecuencia, asi que compartir la forma es lo honesto.
//
// Un motor de masa excentrica no arranca a duty bajo: se queda zumbando sin
// girar. Por eso cada pulso empieza con una patada breve a plena potencia y
// luego cae al nivel pedido. Sin eso, "suave" no existe: o no se mueve, o se
// mueve fuerte.
// ---------------------------------------------------------------------------

void hapticBegin();
void hapticSetEnabled(bool on);
// el aparato se sabe sostenido: los pulsos pueden ser mas largos y mas suaves
void hapticSetHeld(bool held);
// level 0..1 (intensidad sostenida), ms = duracion total incluida la patada
void hapticPulse(float level, uint16_t ms);
// llamar en cada vuelta del loop
void hapticUpdate(uint32_t now);
void hapticStop();
