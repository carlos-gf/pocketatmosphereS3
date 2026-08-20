#pragma once
#include <stdint.h>

// ---------------------------------------------------------------------------
// Lo que el aparato guarda. Dos cosas, y las dos son datos del beholder:
//
//   NOMBRES  cada palabra que alguien le ha dado a un campo, con la profundidad
//            a la que estaba cuando la dio. Esto es divergencia, hecha visible.
//
//   BANDA    cuanto tiempo has pasado en cada profundidad. El histograma es tu
//            banda entre aburrimiento y sobreesfuerzo, medida en vez de supuesta.
//
// Todo cabe en NVS. Se vuelca por USB con el comando DUMP.
// ---------------------------------------------------------------------------

#define NAMES_PER_ATMOS 12
#define NAME_LEN 12
#define BAND_BUCKETS 16

struct NameRec {
  char word[NAME_LEN];  // terminado en 0
  uint8_t depth;        // 0..255 = profundidad de reduccion al nombrarlo
  uint32_t epoch;       // hora RTC, 0 si no habia
};

void storeBegin();

// nombres
uint8_t storeNameCount(uint8_t atmos);
const NameRec &storeName(uint8_t atmos, uint8_t i);
void storeAddName(uint8_t atmos, const char *word, float depth, uint32_t epoch);
void storeClearNames(uint8_t atmos);

// banda de permanencia
void storeDwell(float depth, uint32_t ms);
const uint32_t *storeBand();      // BAND_BUCKETS segundos
uint32_t storeBandTotal();
float storeBandMean();            // profundidad media ponderada por tiempo, -1 si vacia
void storeFlush();                // vuelca lo pendiente (llamar con pantalla apagada)

// ajustes
struct Settings {
  bool sound = true;
  bool reveal = false;   // si revela el lugar de origen tras nombrar
  uint8_t bright = 170;
  uint8_t lastAtmos = 0;
  uint8_t lastDepth = 140;
};
Settings &storeSettings();
void storeSaveSettings();
