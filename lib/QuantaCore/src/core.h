// core.h — hardware-independent logic for the Quanta Controller.
// One box, 4 x 3.5 mm TRS jacks (outputs). Each output drives one light or a piggybacked chain:
//   Altair 100 / 210 : tip = blue channel, ring = white channel
//   Atlas, Helix TS / RCD / MB : one channel on the tip, ring held at 0 V
//   3rd-party equipment: the user decides what tip and ring do
//   Sleeve = ground. All dimming is 0-10 V.
// No Arduino includes, so this is unit-tested on a PC (test/test_core.cpp).
#pragma once
#include <stdint.h>
#include <string.h>

namespace qc {

// ------------------------------------------------------------------ sizes
constexpr uint8_t  JACKS        = 4;
constexpr uint8_t  CHANNELS     = JACKS * 2;   // ch = jack*2 + (0 tip, 1 ring)
constexpr uint8_t  MAX_GROUPS   = 8;            // = CHANNELS, so expert mode can give every channel its own curve
constexpr uint8_t  MAX_POINTS   = 16;
constexpr uint8_t  MAX_PHOTO    = 6;
constexpr uint8_t  NAME_LEN     = 20;
constexpr uint16_t LEVEL_MAX    = 1000;        // levels are permille
constexpr uint16_t DAC_MAX      = 4095;
constexpr uint16_t NO_CLOCK_LEVEL = 250;       // brand-new box with no clock: 25 % so the user sees it works
constexpr uint32_t DAY          = 86400;

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline void copyName(char *dst, const char *src) {
  if (!src) src = "";
  strncpy(dst, src, NAME_LEN - 1); dst[NAME_LEN - 1] = 0;
}

// ------------------------------------------------------------------ fixtures
enum Fixture : uint8_t {
  FX_NONE = 0, FX_ALTAIR_100, FX_ALTAIR_210, FX_ATLAS, FX_HELIX_TS, FX_HELIX_RCD, FX_HELIX_MB, FX_OTHER, FX_COUNT
};
inline bool twoChannel(uint8_t fx) { return fx == FX_ALTAIR_100 || fx == FX_ALTAIR_210; }
inline bool isHelix(uint8_t fx)    { return fx == FX_HELIX_TS || fx == FX_HELIX_RCD || fx == FX_HELIX_MB; }
inline bool isQuanta(uint8_t fx)   { return fx != FX_NONE && fx != FX_OTHER && fx < FX_COUNT; }  // channels & watts locked

// Ids used in the JSON API / GUI.
inline const char *fixtureId(uint8_t fx) {
  static const char *ids[FX_COUNT] = {"none", "altair_100", "altair_210", "atlas", "helix_ts", "helix_rcd", "helix_mb", "other"};
  return fx < FX_COUNT ? ids[fx] : "none";
}
inline uint8_t fixtureFromId(const char *s) {
  if (!s) return FX_NONE;
  for (uint8_t i = 0; i < FX_COUNT; i++) if (!strcmp(s, fixtureId(i))) return i;
  return FX_NONE;
}

// ------------------------------------------------------------------ config
struct Point   { uint16_t min; uint16_t lvl; };             // minute of day, permille
struct Group   { char name[NAME_LEN]; uint8_t color; uint8_t n; Point p[MAX_POINTS]; };
struct Jack    { uint8_t fixture; uint8_t count; char name[NAME_LEN]; uint16_t watts[2]; uint8_t len; };  // watts per light per channel; len = Helix length index
struct Channel { int8_t group; uint8_t trim; uint8_t cal; };  // group -1 = unassigned; trim 0..100 %; cal 80..120 %
struct Photo   { char name[NAME_LEN]; uint16_t minutes; uint16_t lvl[MAX_GROUPS]; };

struct Config {
  uint8_t  nGroups;
  Group    group[MAX_GROUPS];
  Jack     jack[JACKS];
  Channel  ch[CHANNELS];
  uint8_t  nPhoto;
  Photo    photo[MAX_PHOTO];
  uint8_t  intensity;      // schedule scale, 10..100 %
  uint8_t  floorPct;       // lowest non-zero level (Mean Well drivers flicker when very low)
  uint16_t rampSec;        // soft start after a power cut
  uint16_t fadeSec;        // how long a full-range change takes (mode switches, slider moves)
  bool     acclOn;
  uint8_t  acclStartPct;   // acclimation starts at this % of the schedule...
  uint16_t acclDays;       // ...and reaches 100 % after this many days
  uint32_t acclStart;      // epoch seconds (UTC)
  char     tz[48];         // POSIX TZ string
  bool     expert;
  bool     setupDone;
  uint16_t costMils;       // electricity price, 1/1000 $ per kWh (150 = $0.150)
  uint8_t  tank;           // guided: 0 Softies & LPS, 1 Mixed, 2 SPS
  uint8_t  spectrum;       // guided: 0 Shallow Reef, 1 Lagoon Reef, 2 Grow and Pop, 3 Blue, 4 Ultra Blue
  uint16_t offMin;         // guided: lights-off, minute of day (lights-on follows from the tank type)
};

// ------------------------------------------------------------------ product line
// Helix lengths (inches) and wattage per bar.
constexpr uint8_t  HELIX_LENS = 5;
constexpr uint8_t  HELIX_INCH[HELIX_LENS]  = {23, 35, 47, 59, 71};
constexpr uint16_t HELIX_WATTS[HELIX_LENS] = {38, 57, 74, 92, 111};
// Watts per light per channel (side 0 = tip, 1 = ring).
inline uint16_t fixtureWatts(uint8_t fx, uint8_t side, uint8_t len) {
  switch (fx) {
    case FX_ALTAIR_210: return side == 0 ? 150 : 60;
    case FX_ALTAIR_100: return side == 0 ? 70 : 30;
    case FX_ATLAS:      return side == 0 ? 240 : 0;
    case FX_HELIX_TS: case FX_HELIX_RCD: case FX_HELIX_MB:
                        return side == 0 ? HELIX_WATTS[len < HELIX_LENS ? len : 0] : 0;
    default:            return 0;
  }
}

// Guided mode always has these six groups, one per kind of channel, in this order.
// Group colours are fixed: Blue and White are reserved for the Altair channels.
enum GuidedGroup : uint8_t { GG_BLUE = 0, GG_WHITE, GG_ATLAS, GG_TS, GG_RCD, GG_MB, GG_COUNT };
enum Color : uint8_t { COL_BLUE = 0, COL_WHITE, COL_YELLOW, COL_ORANGE, COL_CYAN, COL_INDIGO,
                       COL_GREEN, COL_PINK, COL_PURPLE, COL_RED, COL_COUNT };
inline const char *guidedName(uint8_t g) {
  static const char *n[GG_COUNT] = {"Blue", "White", "Atlas", "Tropic Sun", "Reef Crest", "Meso Blue"};
  return g < GG_COUNT ? n[g] : "Group";
}
inline uint8_t guidedColor(uint8_t g) { return g < GG_COUNT ? g : (uint8_t)COL_GREEN; }   // enum orders match

// Guided channel -> group for a product. 3rd-party equipment starts unassigned (the user picks).
inline int8_t defaultGroup(uint8_t fx, uint8_t side) {
  switch (fx) {
    case FX_ALTAIR_100:
    case FX_ALTAIR_210: return side == 0 ? GG_BLUE : GG_WHITE;     // tip = blue, ring = white
    case FX_ATLAS:      return side == 0 ? GG_ATLAS : -1;
    case FX_HELIX_TS:   return side == 0 ? GG_TS : -1;
    case FX_HELIX_RCD:  return side == 0 ? GG_RCD : -1;
    case FX_HELIX_MB:   return side == 0 ? GG_MB : -1;
    default:            return -1;
  }
}

inline void sortPoints(Group &g) {
  for (uint8_t i = 1; i < g.n; i++) {
    Point k = g.p[i]; int j = i - 1;
    while (j >= 0 && g.p[j].min > k.min) { g.p[j + 1] = g.p[j]; j--; }
    g.p[j + 1] = k;
  }
}

// ------------------------------------------------------------------ guided schedule
// Tank type sets the overall intensity and how long the lights are on (lights-on = lights-off - photoperiod).
enum Tank : uint8_t { TANK_SOFTIE_LPS = 0, TANK_MIXED = 1, TANK_SPS = 2, TANK_COUNT };
inline uint8_t  tankIntensity(uint8_t t)   { return t == TANK_SOFTIE_LPS ? 50 : t == TANK_SPS ? 80 : 65; }
inline uint16_t tankPhotoMin(uint8_t t)    { return t == TANK_SOFTIE_LPS ? 600 : t == TANK_SPS ? 720 : 660; }  // 10 / 11 / 12 h (Luca)
inline uint16_t guidedOnMin(const Config &c) { return (uint16_t)((c.offMin + 1440 - tankPhotoMin(c.tank)) % 1440); }
// Acclimation: start 20 points under the chosen intensity (65 % -> 45 %), as a % of the schedule.
inline uint8_t  acclStartFor(uint8_t intensity) {
  int s = intensity > 20 ? ((intensity - 20) * 100 + intensity / 2) / intensity : 10;
  return (uint8_t)clampi(s, 10, 100);
}

// Spectrum = Luca's blue and white curves for the Altairs.
// Each point is {fraction of the photoperiod x1000, level permille}. 0 outside the points, straight lines between.
enum Spectrum : uint8_t { SP_SHALLOW = 0, SP_LAGOON, SP_GROW_POP, SP_BLUE, SP_ULTRA_BLUE, SP_COUNT };
struct MapPt { uint16_t frac, lvl; };
struct SpectrumMap { uint8_t nw, nb; MapPt w[8], b[8]; };
inline const SpectrumMap &spectrumMap(uint8_t sp) {
  static const SpectrumMap m[SP_COUNT] = {
    // Shallow Reef
    {7, 6, {{100,0},{200,200},{300,1000},{600,1000},{650,500},{700,500},{750,0}},
           {{0,0},{100,200},{250,700},{800,700},{900,400},{1000,0}}},
    // Lagoon Reef
    {8, 8, {{100,0},{200,200},{300,600},{350,1000},{500,1000},{550,400},{650,400},{700,0}},
           {{0,0},{100,400},{200,800},{300,1000},{650,1000},{700,900},{900,800},{1000,0}}},
    // Grow and Pop
    {8, 5, {{0,0},{100,400},{200,1000},{350,1000},{400,500},{500,300},{600,200},{700,0}},
           {{0,0},{100,400},{200,1000},{800,1000},{1000,0}}},
    // Blue
    {4, 5, {{200,0},{300,500},{500,500},{600,0}},
           {{0,0},{100,400},{150,1000},{900,1000},{1000,0}}},
    // Ultra Blue (not for SPS)
    {4, 5, {{200,0},{350,400},{450,400},{550,0}},
           {{0,0},{50,400},{100,1000},{950,1000},{1000,0}}},
  };
  return m[sp < SP_COUNT ? sp : (uint8_t)SP_GROW_POP];
}
// Peak of each channel (permille, before intensity) - used for display.
struct SpectrumShape { uint16_t bluePeak, whitePeak; };
inline SpectrumShape spectrumShape(uint8_t sp) {
  const SpectrumMap &m = spectrumMap(sp);
  SpectrumShape r{0, 0};
  for (uint8_t i = 0; i < m.nb; i++) if (m.b[i].lvl > r.bluePeak)  r.bluePeak  = m.b[i].lvl;
  for (uint8_t i = 0; i < m.nw; i++) if (m.w[i].lvl > r.whitePeak) r.whitePeak = m.w[i].lvl;
  return r;
}
// Stretch a spectrum map over the photoperiod (on .. on+photo minutes).
inline void mapCurve(Group &g, const MapPt *pts, uint8_t n, uint16_t on, uint16_t photo) {
  g.n = n;
  for (uint8_t i = 0; i < n; i++) {
    uint32_t m = ((uint32_t)pts[i].frac * photo + 500) / 1000;
    g.p[i] = {(uint16_t)((on + m) % 1440), pts[i].lvl};
  }
  sortPoints(g);
}

// One ramp-up / hold / ramp-down curve. Blue timing runs on..off; white timing starts later and ends earlier.
inline void rampCurve(Group &g, bool whiteTiming, uint16_t peak, uint16_t on, uint16_t off) {
  int len = ((int)off - (int)on + 1440) % 1440;
  if (len < 240) len = 240;                        // at least 4 h
  auto at = [&](int m) { return (uint16_t)(((int)on + m + 1440) % 1440); };
  int ramp = len >= 600 ? 120 : len / 5;           // 2 h ramps on a 10 h day
  int lead = whiteTiming ? ramp / 2 : 0;
  g.n = 4;
  g.p[0] = {at(lead), 0};               g.p[1] = {at(lead + ramp), peak};
  g.p[2] = {at(len - lead - ramp), peak}; g.p[3] = {at(len - lead), 0};
  sortPoints(g);
}

// Curve of a guided group. Altair blue/white follow Luca's spectrum maps.
// Meso Blue rides with the blue curve; Atlas, Tropic Sun and Reef Crest ride with the white curve.
inline void guidedCurve(Group &g, uint8_t gg, uint8_t spectrum, uint16_t on, uint16_t off) {
  const SpectrumMap &m = spectrumMap(spectrum);
  uint16_t photo = (uint16_t)(((int)off - (int)on + 1440) % 1440);
  if (photo == 0) photo = 1440;
  if (gg == GG_BLUE || gg == GG_MB) mapCurve(g, m.b, m.nb, on, photo);
  else                              mapCurve(g, m.w, m.nw, on, photo);
}

// Rebuild the six guided groups from tank type, spectrum and lights-off.
inline void guidedSchedule(Config &c) {
  c.nGroups = GG_COUNT;
  uint16_t on = guidedOnMin(c);
  for (uint8_t g = 0; g < GG_COUNT; g++) {
    copyName(c.group[g].name, guidedName(g)); c.group[g].color = guidedColor(g);
    guidedCurve(c.group[g], g, c.spectrum, on, c.offMin);
  }
}

// ------------------------------------------------------------------ curves

// Linear interpolation between points, wrapping at midnight. Seconds for smooth ramps.
inline uint16_t curveAt(const Group &g, uint32_t secOfDay) {
  if (g.n == 0) return 0;
  if (g.n == 1) return g.p[0].lvl;
  int32_t t = (int32_t)(secOfDay % DAY);
  int8_t a = g.n - 1;
  for (uint8_t i = 0; i < g.n; i++) if ((int32_t)g.p[i].min * 60 <= t) a = i;
  uint8_t b = (a + 1) % g.n;
  int32_t ta = g.p[a].min * 60, tb = g.p[b].min * 60;
  if (tb <= ta) tb += DAY;
  int32_t tt = t < ta ? t + (int32_t)DAY : t;
  int32_t span = tb - ta;
  int64_t f = span ? (int64_t)(tt - ta) * 1000 / span : 0;
  return (uint16_t)(g.p[a].lvl + ((int32_t)g.p[b].lvl - (int32_t)g.p[a].lvl) * f / 1000);
}

// Acclimation factor in permille for a given time.
inline uint16_t acclFactor(const Config &c, uint32_t epoch) {
  if (!c.acclOn || c.acclDays == 0) return 1000;
  if (epoch <= c.acclStart) return c.acclStartPct * 10;
  uint32_t el = epoch - c.acclStart, tot = (uint32_t)c.acclDays * DAY;
  if (el >= tot) return 1000;
  return (uint16_t)(c.acclStartPct * 10 + (uint64_t)(1000 - c.acclStartPct * 10) * el / tot);
}

// ------------------------------------------------------------------ modes
enum Mode : uint8_t { MODE_AUTO = 0, MODE_MANUAL = 1, MODE_PHOTO = 2 };
struct Override {
  uint8_t  mode = MODE_AUTO;
  uint32_t until = 0;              // epoch it ends; 1 = clock unknown (the uptime counter ends it)
  int8_t   photo = -1;             // which picture setting is showing
  uint8_t  mins = 0;               // 30 or 60: how long it lasts
  uint16_t lvl[MAX_GROUPS] = {0};
};

// Manual and picture mode only ever last 30 min or 1 h, so nobody forgets and cooks their corals.
inline uint8_t overrideMinutes(int m) { return m > 30 ? 60 : 30; }

// Is the override still in force? Both modes always have an end.
inline bool overrideActive(const Override &o, bool timeValid, uint32_t epoch) {
  if (o.mode == MODE_AUTO) return false;
  if (o.until == 0) return false;                // no end recorded: not allowed any more
  if (!timeValid) return true;                   // can't tell time: keep what the user chose
  return epoch < o.until;
}

// Levels (permille) for every group right now. Returns the mode in force.
inline uint8_t groupLevels(const Config &c, const Override &o, bool timeValid, uint32_t epoch,
                           uint32_t secOfDay, uint16_t out[MAX_GROUPS]) {
  if (overrideActive(o, timeValid, epoch)) {
    for (uint8_t g = 0; g < MAX_GROUPS; g++) out[g] = o.lvl[g] > LEVEL_MAX ? LEVEL_MAX : o.lvl[g];
    return o.mode;
  }
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    if (g >= c.nGroups) { out[g] = 0; continue; }
    if (!timeValid) { out[g] = NO_CLOCK_LEVEL; continue; }
    uint32_t v = curveAt(c.group[g], secOfDay);
    v = v * c.intensity / 100;
    v = v * acclFactor(c, epoch) / 1000;
    out[g] = (uint16_t)(v > LEVEL_MAX ? LEVEL_MAX : v);
  }
  return MODE_AUTO;
}

inline bool channelInUse(const Config &c, uint8_t ch) {
  if (ch >= CHANNELS) return false;
  uint8_t fx = c.jack[ch / 2].fixture;
  if (fx == FX_NONE) return false;
  if (fx == FX_OTHER) return c.ch[ch].group >= 0;    // 3rd party: only the sides the user assigned
  if ((ch & 1) && !twoChannel(fx)) return false;     // single-channel products: ring held at 0 V
  return true;
}

// Final level of one output in permille, after trim and the minimum-dim floor.
inline uint16_t channelLevel(const Config &c, const uint16_t grp[MAX_GROUPS], uint8_t ch) {
  if (!channelInUse(c, ch)) return 0;
  int8_t g = c.ch[ch].group;
  if (g < 0 || g >= c.nGroups) return 0;
  uint32_t v = (uint32_t)grp[g] * c.ch[ch].trim / 100;
  if (v == 0) return 0;
  uint32_t fl = (uint32_t)c.floorPct * 10;
  if (v < fl) v = fl;
  return (uint16_t)(v > LEVEL_MAX ? LEVEL_MAX : v);
}

// Output hardware: DAC 0..2.048 V (12-bit, internal ref, gain 1) -> op-amp gain 4.9 -> 0..10.03 V.
// cal (80..120 %) trims each output so 100 % reads exactly 10.00 V on a meter.
constexpr float DAC_FS_V = 2.048f, AMP_GAIN = 4.9f;           // 39k / 10k + 1
constexpr float OUT_FS_V = DAC_FS_V * AMP_GAIN;                  // 10.035 V at code 4095
inline uint16_t levelToCode(uint16_t lvl, uint8_t cal) {
  if (lvl == 0) return 0;
  float v = lvl / 1000.0f * 10.0f * cal / 100.0f;                // wanted volts
  int code = (int)(v / OUT_FS_V * DAC_MAX + 0.5f);
  return (uint16_t)clampi(code, 0, DAC_MAX);
}
inline uint16_t codeToMilliVolts(uint16_t code) { return (uint16_t)(code * OUT_FS_V * 1000.0f / DAC_MAX + 0.5f); }

// Move `cur` toward `tgt` by at most fullRange/fadeMs per ms. Levels are permille.
inline uint16_t slew(uint16_t cur, uint16_t tgt, uint32_t dtMs, uint32_t fadeMs) {
  if (fadeMs == 0 || cur == tgt) return tgt;
  uint32_t step = (uint32_t)LEVEL_MAX * dtMs / fadeMs;
  if (step == 0) step = 1;
  if (tgt > cur) return (uint16_t)((uint32_t)(tgt - cur) <= step ? tgt : cur + step);
  return (uint16_t)((uint32_t)(cur - tgt) <= step ? tgt : cur - step);
}

// ------------------------------------------------------------------ output monitoring
// Each output has a 1k resistor between the op-amp and the jack; the op-amp's feedback comes from
// the jack side, so the jack voltage is correct regardless of load. Measuring the op-amp side tells
// us the current the fixtures push back into the controller (Mean Well dim inputs source ~0.1-0.2 mA)
// and whether the jack is shorted (the op-amp runs to its rail trying to raise a dead-short jack).
enum OutStatus : uint8_t { OUT_IDLE = 0, OUT_OK = 1, OUT_NO_LOAD = 2, OUT_SHORT = 3, OUT_UNKNOWN = 4 };
constexpr uint16_t SERIES_OHMS = 1000;
struct OutReading { uint8_t status; int16_t microAmps; };
inline OutReading classifyOutput(uint16_t setMv, uint16_t ampMv, bool inUse) {
  OutReading r{OUT_IDLE, 0};
  int32_t ua = ((int32_t)setMv - (int32_t)ampMv) * 1000 / SERIES_OHMS;   // + = fixture sourcing
  r.microAmps = (int16_t)clampi(ua, -32000, 32000);
  if (!inUse) return r;
  if (ampMv > setMv + 1500 && setMv >= 500) { r.status = OUT_SHORT; return r; }
  if (setMv < 1000 || setMv > 9000) { r.status = OUT_UNKNOWN; return r; } // drivers only measurable mid-range
  r.status = ua >= 40 ? OUT_OK : OUT_NO_LOAD;
  return r;
}

// ------------------------------------------------------------------ defaults & validation
inline void defaultConfig(Config &c) {
  memset(&c, 0, sizeof(c));
  c.tank = TANK_MIXED; c.spectrum = SP_GROW_POP; c.offMin = 21 * 60;
  guidedSchedule(c);
  for (uint8_t j = 0; j < JACKS; j++) {
    c.jack[j].fixture = FX_NONE; c.jack[j].count = 1;
    char n[NAME_LEN] = "Output 1"; n[7] = (char)('1' + j); copyName(c.jack[j].name, n);
  }
  for (uint8_t i = 0; i < CHANNELS; i++) c.ch[i] = {-1, 100, 100};
  for (uint8_t p = 0; p < MAX_PHOTO; p++) c.photo[p].minutes = 30;
  c.nPhoto = 2;
  // per guided group:               Blue  White Atlas  TS   RCD   MB
  static const uint16_t glow[GG_COUNT] = {1000, 80,  250,  150, 250, 1000};
  static const uint16_t tru[GG_COUNT]  = {500, 1000, 1000, 1000, 1000, 500};
  copyName(c.photo[0].name, "Coral glow"); memcpy(c.photo[0].lvl, glow, sizeof(glow));
  copyName(c.photo[1].name, "True color"); memcpy(c.photo[1].lvl, tru, sizeof(tru));
  c.intensity = tankIntensity(c.tank); c.floorPct = 10; c.rampSec = 60; c.fadeSec = 3;
  c.acclOn = false; c.acclStartPct = acclStartFor(c.intensity); c.acclDays = 30; c.acclStart = 0;
  strcpy(c.tz, "CST6CDT,M3.2.0,M11.1.0");
  c.expert = false; c.setupDone = false; c.costMils = 150;
}

// Apply a product to a jack: channels and watts fill in by themselves.
inline void setFixture(Config &c, uint8_t j, uint8_t fx, uint8_t len = 0) {
  if (j >= JACKS) return;
  Jack &J = c.jack[j];
  J.fixture = fx < FX_COUNT ? fx : (uint8_t)FX_NONE;
  J.len = isHelix(J.fixture) && len < HELIX_LENS ? len : 0;
  for (uint8_t s = 0; s < 2; s++) {
    int8_t g = defaultGroup(J.fixture, s);
    c.ch[j * 2 + s].group = (g >= 0 && g < c.nGroups) ? g : -1;
    if (J.fixture != FX_OTHER) J.watts[s] = fixtureWatts(J.fixture, s, J.len);
  }
}

// Make any config safe to run: clamp everything, fix references, sort curves.
// Returns the number of fields it had to correct (0 = config was already clean).
inline int sanitize(Config &c) {
  int fixes = 0;
  auto fix = [&](bool bad) { if (bad) fixes++; return bad; };
  if (fix(c.nGroups < 1 || c.nGroups > MAX_GROUPS)) c.nGroups = c.nGroups < 1 ? 1 : MAX_GROUPS;
  for (uint8_t g = 0; g < MAX_GROUPS; g++) {
    Group &G = c.group[g];
    G.name[NAME_LEN - 1] = 0;
    if (fix(G.n > MAX_POINTS)) G.n = MAX_POINTS;
    for (uint8_t i = 0; i < G.n; i++) {
      if (fix(G.p[i].min > 1439)) G.p[i].min = 1439;
      if (fix(G.p[i].lvl > LEVEL_MAX)) G.p[i].lvl = LEVEL_MAX;
    }
    sortPoints(G);
    if (fix(G.color >= COL_COUNT)) G.color = COL_GREEN;
  }
  for (uint8_t j = 0; j < JACKS; j++) {
    c.jack[j].name[NAME_LEN - 1] = 0;
    if (fix(c.jack[j].fixture >= FX_COUNT)) c.jack[j].fixture = FX_NONE;
    if (fix(c.jack[j].count < 1 || c.jack[j].count > 8)) c.jack[j].count = c.jack[j].count < 1 ? 1 : 8;
    if (fix(c.jack[j].len >= HELIX_LENS || (c.jack[j].len && !isHelix(c.jack[j].fixture)))) c.jack[j].len = 0;
    for (uint8_t s = 0; s < 2; s++) {
      uint8_t fx = c.jack[j].fixture;
      if (fx == FX_OTHER) { if (fix(c.jack[j].watts[s] > 1000)) c.jack[j].watts[s] = 1000; }
      else if (fix(c.jack[j].watts[s] != fixtureWatts(fx, s, c.jack[j].len))) c.jack[j].watts[s] = fixtureWatts(fx, s, c.jack[j].len);
    }
  }
  if (fix(c.tank >= TANK_COUNT)) c.tank = TANK_MIXED;
  if (fix(c.spectrum >= SP_COUNT)) c.spectrum = SP_GROW_POP;
  if (fix(c.offMin > 1439)) c.offMin = 1439;
  // Guided mode: the six fixed groups exist and Quanta products sit on their own group.
  if (!c.expert) {
    // Guided curves always come from tank + spectrum + lights-off, so rebuild them
    // (this also moves boards saved with older firmware onto the current curves).
    Group before[GG_COUNT]; memcpy(before, c.group, sizeof(before));
    uint8_t nBefore = c.nGroups;
    guidedSchedule(c);
    fix(nBefore != GG_COUNT || memcmp(before, c.group, sizeof(before)) != 0);
    for (uint8_t i = 0; i < CHANNELS; i++) {
      uint8_t fx = c.jack[i / 2].fixture;
      if (isQuanta(fx) && fix(c.ch[i].group != defaultGroup(fx, i & 1))) c.ch[i].group = defaultGroup(fx, i & 1);
    }
  }
  for (uint8_t i = 0; i < CHANNELS; i++) {
    if (fix(c.ch[i].group >= (int8_t)c.nGroups || c.ch[i].group < -1)) c.ch[i].group = -1;
    if (fix(c.ch[i].trim > 100)) c.ch[i].trim = 100;
    if (fix(c.ch[i].cal < 80 || c.ch[i].cal > 120)) c.ch[i].cal = (uint8_t)clampi(c.ch[i].cal, 80, 120);
  }
  if (fix(c.nPhoto > MAX_PHOTO)) c.nPhoto = MAX_PHOTO;
  for (uint8_t p = 0; p < MAX_PHOTO; p++) {
    c.photo[p].name[NAME_LEN - 1] = 0;
    if (fix(c.photo[p].minutes != 30 && c.photo[p].minutes != 60)) c.photo[p].minutes = overrideMinutes(c.photo[p].minutes);
    for (uint8_t g = 0; g < MAX_GROUPS; g++) if (fix(c.photo[p].lvl[g] > LEVEL_MAX)) c.photo[p].lvl[g] = LEVEL_MAX;
  }
  if (fix(c.intensity < 10 || c.intensity > 100)) c.intensity = (uint8_t)clampi(c.intensity, 10, 100);
  if (fix(c.floorPct > 40)) c.floorPct = 40;
  if (fix(c.rampSec > 1800)) c.rampSec = 1800;
  if (fix(c.fadeSec > 60)) c.fadeSec = 60;
  if (fix(c.acclStartPct < 10 || c.acclStartPct > 100)) c.acclStartPct = (uint8_t)clampi(c.acclStartPct, 10, 100);
  if (fix(c.acclDays < 1 || c.acclDays > 120)) c.acclDays = (uint16_t)clampi(c.acclDays, 1, 120);
  c.tz[sizeof(c.tz) - 1] = 0;
  if (fix(c.tz[0] == 0)) strcpy(c.tz, "UTC0");
  return fixes;
}

// ------------------------------------------------------------------ misc
inline uint32_t crc32(const uint8_t *d, uint32_t n, uint32_t crc = 0) {
  crc = ~crc;
  while (n--) { crc ^= *d++; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1))); }
  return ~crc;
}

}  // namespace qc
