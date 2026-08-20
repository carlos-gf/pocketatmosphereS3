#include "store.h"
#include "atmos.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

static Preferences prefs;
static NameRec gNames[ATMOS_COUNT][NAMES_PER_ATMOS];
static uint8_t gCount[ATMOS_COUNT];
static uint32_t gBand[BAND_BUCKETS];
static Settings gSet;

// El tiempo de permanencia se acumula en RAM y solo se vuelca cuando la pantalla
// se apaga: escribir NVS congela ~1 s los dos nucleos y cortaria la deriva.
static bool gBandDirty = false;
static uint32_t gPendingMs[BAND_BUCKETS];

static void keyFor(char *out, size_t n, const char *p, int i) { snprintf(out, n, "%s%d", p, i); }

void storeBegin() {
  prefs.begin("pocketatm", false);

  for (int a = 0; a < ATMOS_COUNT; a++) {
    char k[8];
    keyFor(k, sizeof k, "n", a);
    size_t got = prefs.getBytes(k, gNames[a], sizeof(gNames[a]));
    gCount[a] = 0;
    if (got == sizeof(gNames[a])) {
      for (int i = 0; i < NAMES_PER_ATMOS; i++)
        if (gNames[a][i].word[0]) gCount[a] = i + 1;
    } else {
      memset(gNames[a], 0, sizeof(gNames[a]));
    }
  }

  if (prefs.getBytes("band", gBand, sizeof(gBand)) != sizeof(gBand)) memset(gBand, 0, sizeof(gBand));
  memset(gPendingMs, 0, sizeof(gPendingMs));

  gSet.sound = prefs.getBool("snd", true);
  gSet.haptics = prefs.getBool("hap", true);
  gSet.reveal = prefs.getBool("rev", false);
  gSet.bright = prefs.getUChar("bri", 170);
  gSet.lastAtmos = prefs.getUChar("la", 0);
  gSet.lastDepth = prefs.getUChar("ld", 140);
  if (gSet.lastAtmos >= ATMOS_COUNT) gSet.lastAtmos = 0;
}

uint8_t storeNameCount(uint8_t a) { return a < ATMOS_COUNT ? gCount[a] : 0; }

const NameRec &storeName(uint8_t a, uint8_t i) {
  static NameRec empty = {};
  if (a >= ATMOS_COUNT || i >= NAMES_PER_ATMOS) return empty;
  return gNames[a][i];
}

void storeAddName(uint8_t a, const char *word, float depth, uint32_t epoch) {
  if (a >= ATMOS_COUNT || !word || !word[0]) return;

  // lista llena: se descarta la mas antigua (indice 0) y se desplaza
  if (gCount[a] >= NAMES_PER_ATMOS) {
    memmove(&gNames[a][0], &gNames[a][1], sizeof(NameRec) * (NAMES_PER_ATMOS - 1));
    gCount[a] = NAMES_PER_ATMOS - 1;
  }
  NameRec &r = gNames[a][gCount[a]];
  memset(&r, 0, sizeof r);
  snprintf(r.word, NAME_LEN, "%s", word);
  int d = (int)(depth * 255.0f + 0.5f);
  r.depth = (uint8_t)(d < 0 ? 0 : (d > 255 ? 255 : d));
  r.epoch = epoch;
  gCount[a]++;

  char k[8];
  keyFor(k, sizeof k, "n", a);
  prefs.putBytes(k, gNames[a], sizeof(gNames[a]));
}

void storeClearNames(uint8_t a) {
  if (a >= ATMOS_COUNT) return;
  memset(gNames[a], 0, sizeof(gNames[a]));
  gCount[a] = 0;
  char k[8];
  keyFor(k, sizeof k, "n", a);
  prefs.putBytes(k, gNames[a], sizeof(gNames[a]));
}

void storeDwell(float depth, uint32_t ms) {
  if (ms == 0) return;
  int b = (int)(depth * (BAND_BUCKETS - 1) + 0.5f);
  if (b < 0) b = 0;
  if (b >= BAND_BUCKETS) b = BAND_BUCKETS - 1;
  gPendingMs[b] += ms;
  if (gPendingMs[b] >= 1000) {
    gBand[b] += gPendingMs[b] / 1000;
    gPendingMs[b] %= 1000;
    gBandDirty = true;
  }
}

const uint32_t *storeBand() { return gBand; }

uint32_t storeBandTotal() {
  uint32_t t = 0;
  for (int i = 0; i < BAND_BUCKETS; i++) t += gBand[i];
  return t;
}

float storeBandMean() {
  uint32_t t = storeBandTotal();
  if (t == 0) return -1.0f;
  double acc = 0;
  for (int i = 0; i < BAND_BUCKETS; i++)
    acc += (double)gBand[i] * ((double)i / (BAND_BUCKETS - 1));
  return (float)(acc / t);
}

void storeFlush() {
  if (gBandDirty) {
    prefs.putBytes("band", gBand, sizeof(gBand));
    gBandDirty = false;
  }
}

Settings &storeSettings() { return gSet; }

void storeSaveSettings() {
  prefs.putBool("snd", gSet.sound);
  prefs.putBool("hap", gSet.haptics);
  prefs.putBool("rev", gSet.reveal);
  prefs.putUChar("bri", gSet.bright);
  prefs.putUChar("la", gSet.lastAtmos);
  prefs.putUChar("ld", gSet.lastDepth);
}
