#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// El zumbador es un canal pobre: una onda cuadrada, una nota cada vez, sin
// timbre ni ruido. Tratarlo como fuente de "ambiente" (un zumbido continuo) lo
// convierte en un electrodomestico y destruye el campo.
//
// Asi que se usa como PUNTUACION, no como fondo: eventos escasos, con envolvente
// de amplitud hecha variando el ciclo de trabajo del LEDC (la potencia de una
// cuadrada depende del duty, asi que un duty que sube y baja suena como algo que
// aparece y se va, no como un pitido).
//
// Los eventos salen a intervalos aleatorios, con la frecuencia media dada por el
// ambiente y aumentada por la agitacion del IMU.
// ---------------------------------------------------------------------------

void buzzBegin();
void buzzSetEnabled(bool on);
// llamar en cada vuelta del loop: hace avanzar la envolvente y decide eventos
void buzzUpdate(uint32_t now, uint16_t centrePitch, uint8_t spreadSemitones,
                uint8_t ratePerMin, float agitation, float depth);
// evento inmediato (confirmaciones de interfaz)
void buzzPing(uint16_t hz, uint16_t ms);
void buzzSilence();
// recorre la banda util del altavoz e informa por el callback
void buzzSelfTest(void (*say)(const char *));
