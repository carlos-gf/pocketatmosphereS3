#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// LA OPERACION DE LA TESIS (sketch_rpc_depth3_texture.pde), en el aparato.
//
// Sustituye a la permutacion LOCAL por celdas de field.cpp. Aquella conservaba
// el histograma y salvaba los acentos saturados, pero NO es la operacion que se
// reporta en el capitulo 5. Esta si, constante por constante:
//
//   clave de destino = luminancia desenfocada, sigma(d) = 3.5 + 31.5*d^1.3
//                      (cotizada a 900 px y escalada con el ancho)
//   clave de origen  = indice de banda de luminancia + tono dentro de la banda,
//                      con Q(d) = 2^(6 - 5d)
//   y despues, DENTRO de cada banda, las posiciones de destino se reordenan por
//   un campo de tono desenfocado, promediado como VECTOR y pesado por croma.
//   Ese ultimo paso es lo que impide que el color se deshaga en hilos y la
//   imagen se vaya a un lavado oliva.
//
//   TEXTURE mueve la pertenencia a la banda:
//       banda = floor( (L + TEXTURE*(ruido - 0.5)) * Q )
//   sin eso las bandas salen planas y con anillos concentricos.
//
// DOS COSAS CAMBIAN respecto al sketch, ambas medidas, ninguna por gusto:
//
//  1. TEXTURE = 0.10, no 0.25. Los estimulos se generan a 900 px y se MIRAN
//     reducidos, y el ruido de TEXTURE es por pixel: al reducir se promedia y
//     desaparece. Energia tonal de escala fina (desviacion de la luminancia
//     menos su media 3x3), referencia 900->412 = 5.9 / 6.4 / 8.3 en d =
//     0.2/0.5/0.8; a 412 nativo TEXTURE 0.25 da 14.3 / 15.2 / 19.2 y TEXTURE
//     0.10 da 7.3 / 9.2 / 15.7. En d=0.8 no hay valor que acierte: con Q=4 la
//     propia ordenacion genera estructura fina. Eso es del tamano, no del knob.
//
//  2. Se calcula a 206x206 y se dobla a 412 por VECINO MAS PROXIMO. A media
//     resolucion el resultado es indistinguible (sigma ya es >= 7 px) y cuesta
//     cuatro veces menos. Y el doblado tosco resulta ser el CORRECTO: medido,
//     206 + vecino da 5.99 frente a 5.89 de la referencia, mientras que 206 +
//     bilineal se queda en 1.92. La interpolacion suave se come justo el grano
//     que la referencia tiene.
//
// ---------------------------------------------------------------------------
// LA RESPIRACION
//
// La profundidad oscila entre dos valores. No se puede hacer suave, y conviene
// saber por que antes de intentar arreglarlo: Q es un ENTERO y las anchuras de
// las cajas del desenfoque tambien. Medido a 206 px, cambiar sigma menos de un
// 5% no cambia ni un pixel, y mover Q en 1 -o en 0.125, da igual- reordena la
// imagen entera. Con Q fraccionario tampoco se subdivide: 5.000 -> 5.125 da
// mean|diff| 16.9 y 5 -> 6 da 20.4. La operacion no es continua en d.
//
// Pero SI es un cambio denso, no un centelleo: en ese paso el 67% de los
// pixeles se mueve algo y solo el 1,7% se mueve mucho -el mismo reparto que un
// desplazamiento rigido de 1,5 px-. Asi que en vez de fingir suavidad, la
// animacion da UN FOTOGRAMA POR CUANTO: se enumeran los estados distintos
// (Q, cajas) del recorrido y cada uno es un fotograma. Ni fotogramas muertos ni
// saltos dobles: desviacion de 2,4 frente a 6,1 repartiendo d uniformemente.
//
// Salen unos 30 estados entre d=0.30 y d=0.75. Se calculan UNA vez, se guardan
// (30 x 206 x 206 x 2 = 2,55 MB en PSRAM) y a partir de ahi la reproduccion es
// un volcado: el ciclo va y vuelve, y el tempo es libre porque ya no se calcula
// nada. A ~330 ms por fotograma la respiracion completa dura unos 19 s.
//
// Se construye PEREZOSAMENTE, un estado por vuelta de loop(), y mientras tanto
// se respira con los que ya existen. El aparato nunca se queda parado esperando.
// ---------------------------------------------------------------------------

// Nivel FINO: lo que se mira. 206*206 = 42436 < 65536, asi que los indices de
// la ordenacion caben en uint16 y el trafico a PSRAM se parte por la mitad.
#define RPC_W 206
#define RPC_H 206
#define RPC_N ((int32_t)RPC_W * RPC_H)
// Nivel GRUESO: lo que se ve mientras el pulgar arrastra. La cuarta parte del
// trabajo, y a simple vista apenas se distingue: el campo ya esta desenfocado.
#define RPC_CW 103
#define RPC_CH 103
#define RPC_TEXTURE 0.10f
#define RPC_STATES_MAX 48
#define RPC_COARSE_SLOTS 40   // el recorrido entero tiene 63 estados a 103 px
#define RPC_NOISE_SEED 12345

// Milisegundos por etapa de la ultima reduccion, medidos EN LA PLACA. El
// comando TIME por USB los imprime: "va lento" no es un diagnostico.
struct RpcTiming {
  uint16_t w = 0, blur = 0, keys = 0, sort = 0, scatter = 0, total = 0;
};
const RpcTiming &rpcTiming();

// Mapa profundidad -> parametros, identico al sketch.
int   rpcQForDepth(float d);
float rpcSigmaForDepth(float d, int w);

// Una reduccion a pantalla completa, en el nivel fino. Cacheada por estado: si
// (ambiente, Q, cajas) no ha cambiado, esto es solo un volcado.
bool rpcStill(uint16_t *out412, int atmos, float depth);

// Lo mismo en el nivel grueso, con una cache de todo el recorrido. Es lo que se
// usa MIENTRAS SE ARRASTRA, y es lo que hace que el pulgar mueva algo vivo.
bool rpcDrag(uint16_t *out412, int atmos, float depth);

// --- respiracion -------------------------------------------------------------
// Fija el recorrido y el ambiente. Si cambia algo, tira la cache.
void rpcBreathSet(int atmos, float lo, float hi);
// Construye COMO MUCHO un estado y vuelve. Llamar desde loop(). Devuelve true
// mientras quede trabajo.
bool rpcBreathBuild();
// Cuantos estados hay listos, y cuantos habra en total.
int  rpcBreathReady();
int  rpcBreathTotal();
// Vuelca el fotograma que toca en este instante (ida y vuelta por el ciclo).
// ms_per_frame fija el tempo. Devuelve false si aun no hay nada que enseñar.
bool rpcBreathFrame(uint16_t *out412, uint32_t now_ms, uint16_t ms_per_frame);
// Suelta la cache (al cambiar de ambiente, o si hace falta memoria).
void rpcBreathFree();

// Invalida las caches de fotograma sin soltar la memoria. Solo lo usa TIME:
// cronometrar un volcado de cache mediria cero y no diria nada.
void rpcForget();
