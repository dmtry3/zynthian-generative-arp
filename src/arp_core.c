#include "arp_core.h"
#include <math.h>
#include <string.h>

#define ARP_MAX_PAT (ARP_MAX_NOTES * ARP_MAX_OCTAVES)

static const int rate_ticks[ARP_NRATES] = { 960, 480, 320, 240, 160, 120 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void arp_init(ArpCore* a) {
  memset(a, 0, sizeof *a);
  a->rng = 0x9E3779B9u;
  arp_set_params(a, ARP_RATE_16, 50, ARP_UP, 1);
  arp_set_length(a, ARP_MAX_STEPS);
  for (int i = 0; i < ARP_MAX_STEPS; i++) arp_set_step(a, i, 100, 0, 100, ARP_GATE_GLOBAL);
}

void arp_set_lane_length(ArpCore* a, int lane, int length) {
  if (lane >= 0 && lane < ARP_NLANES) a->lane_len[lane] = clampi(length, 1, ARP_MAX_STEPS);
}

void arp_set_length(ArpCore* a, int length) {
  for (int r = 0; r < ARP_NLANES; r++) arp_set_lane_length(a, r, length);
}

void arp_set_step(ArpCore* a, int i, int prob, int shift, int vel, int gate) {
  if (i < 0 || i >= ARP_MAX_STEPS) return;
  ArpStep* st = &a->steps[i];
  st->prob = clampi(prob, 0, 100);
  st->shift = clampi(shift, -24, 24);
  st->vel = clampi(vel, 0, 200);
  st->gate = (gate <= ARP_GATE_GLOBAL) ? ARP_GATE_GLOBAL : clampi(gate, 5, ARP_GATE_TIE);
}

void arp_set_params(ArpCore* a, int rate, int gate_pct, int order, int octaves) {
  a->rate = clampi(rate, 0, ARP_NRATES - 1);
  a->gate_pct = clampi(gate_pct, 5, 100);
  a->order = clampi(order, 0, ARP_NORDERS - 1);
  a->octaves = clampi(octaves, 1, ARP_MAX_OCTAVES);
  a->step_ticks = rate_ticks[a->rate];
  a->gate_ticks = a->step_ticks * a->gate_pct / 100;
  if (a->gate_ticks < 1) a->gate_ticks = 1;
}

void arp_seed(ArpCore* a, uint32_t seed) { a->rng = seed ? seed : 0x9E3779B9u; }

static uint32_t rnd(ArpCore* a) {   /* xorshift32 */
  uint32_t x = a->rng;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return a->rng = x;
}

static int key_less(uint8_t n1, uint8_t c1, uint8_t n2, uint8_t c2) {
  return n1 != n2 ? n1 < n2 : c1 < c2;
}

static void reset_order(ArpCore* a) { a->has_last = 0; a->played_idx = 0; a->count = 0; }

static int find(const ArpNote* v, int n, uint8_t ch, uint8_t note) {
  for (int i = 0; i < n; i++) if (v[i].ch == ch && v[i].note == note) return i;
  return -1;
}

static void held_add(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel) {
  int i = find(a->held, a->nheld, ch, note);
  if (i >= 0) { a->held[i].vel = vel; return; }
  if (a->nheld >= ARP_MAX_NOTES) return;
  int pos = a->nheld;
  while (pos > 0 && key_less(note, ch, a->held[pos - 1].note, a->held[pos - 1].ch)) {
    a->held[pos] = a->held[pos - 1];
    pos--;
  }
  a->held[pos].ch = ch; a->held[pos].note = note; a->held[pos].vel = vel;
  a->held[pos].seq = a->press_seq++;
  a->nheld++;
}

static void held_remove(ArpCore* a, uint8_t ch, uint8_t note) {
  int i = find(a->held, a->nheld, ch, note);
  if (i < 0) return;
  memmove(&a->held[i], &a->held[i + 1], (size_t)(a->nheld - i - 1) * sizeof(ArpNote));
  a->nheld--;
  if (a->nheld == 0) reset_order(a);
}

int arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel) {
  int first = (a->nheld == 0);
  if (a->latch && a->nphys == 0 && a->nheld > 0) {   /* new chord replaces the latched one */
    a->nheld = 0;
    reset_order(a);
  }
  if (find(a->phys, a->nphys, ch, note) < 0 && a->nphys < ARP_MAX_NOTES) {
    a->phys[a->nphys].ch = ch; a->phys[a->nphys].note = note; a->phys[a->nphys].vel = vel;
    a->nphys++;
  }
  held_add(a, ch, note, vel);
  return first;
}

void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note) {
  int i = find(a->phys, a->nphys, ch, note);
  if (i >= 0) a->phys[i] = a->phys[--a->nphys];
  if (!a->latch) held_remove(a, ch, note);
}

void arp_set_latch(ArpCore* a, int on) {
  on = on ? 1 : 0;
  if (on == a->latch) return;
  a->latch = on;
  if (!on)   /* keep only the keys that are still down */
    for (int i = a->nheld - 1; i >= 0; i--)
      if (find(a->phys, a->nphys, a->held[i].ch, a->held[i].note) < 0) held_remove(a, a->held[i].ch, a->held[i].note);
}

void arp_release_all(ArpCore* a) { a->nheld = 0; a->nphys = 0; reset_order(a); }

/* Held keys repeated over the octaves, notes above 127 dropped.
 * by_pitch: sorted by pitch (duplicates removed); otherwise octave by octave in press order. */
static int build_pattern(const ArpCore* a, ArpNote* p, int by_pitch) {
  int m = 0;
  if (by_pitch) {
    for (int o = 0; o < a->octaves; o++)
      for (int i = 0; i < a->nheld; i++) {
        int pitch = a->held[i].note + 12 * o;
        if (pitch > 127) continue;
        ArpNote n = a->held[i];
        n.note = (uint8_t)pitch;
        int pos = m;
        while (pos > 0 && key_less(n.note, n.ch, p[pos - 1].note, p[pos - 1].ch)) { p[pos] = p[pos - 1]; pos--; }
        p[pos] = n;
        m++;
      }
    int w = 0;   /* remove duplicates (same pitch and channel from different octaves) */
    for (int i = 0; i < m; i++)
      if (w == 0 || p[i].note != p[w - 1].note || p[i].ch != p[w - 1].ch) p[w++] = p[i];
    return w;
  }
  ArpNote ord[ARP_MAX_NOTES];
  int n = a->nheld;
  for (int i = 0; i < n; i++) {   /* held keys in press order */
    int pos = i;
    while (pos > 0 && ord[pos - 1].seq > a->held[i].seq) { ord[pos] = ord[pos - 1]; pos--; }
    ord[pos] = a->held[i];
  }
  for (int o = 0; o < a->octaves; o++)
    for (int i = 0; i < n; i++) {
      int pitch = ord[i].note + 12 * o;
      if (pitch > 127) continue;
      p[m] = ord[i];
      p[m].note = (uint8_t)pitch;
      m++;
    }
  return m;
}

/* index of the first entry above / last entry below the last played pitch, -1 if none */
static int above(const ArpCore* a, const ArpNote* p, int m) {
  for (int i = 0; i < m; i++) if (key_less(a->last_note, a->last_ch, p[i].note, p[i].ch)) return i;
  return -1;
}
static int below(const ArpCore* a, const ArpNote* p, int m) {
  for (int i = m - 1; i >= 0; i--) if (key_less(p[i].note, p[i].ch, a->last_note, a->last_ch)) return i;
  return -1;
}

static int pick(ArpCore* a, const ArpNote* p, int m) {
  int i;
  switch (a->order) {
  case ARP_DOWN:
    i = a->has_last ? below(a, p, m) : -1;
    return i >= 0 ? i : m - 1;
  case ARP_UPDOWN:
  case ARP_DOWNUP:
    if (!a->has_last) { a->dir = (a->order == ARP_UPDOWN) ? 1 : -1; return a->dir > 0 ? 0 : m - 1; }
    if (a->dir == 0) a->dir = 1;
    i = a->dir > 0 ? above(a, p, m) : below(a, p, m);
    if (i < 0) { a->dir = -a->dir; i = a->dir > 0 ? above(a, p, m) : below(a, p, m); }
    return i >= 0 ? i : 0;
  case ARP_PLAYED:
    return (int)(a->played_idx++ % (uint32_t)m);
  case ARP_RANDOM: {
    int k = -1;
    if (a->has_last)
      for (int j = 0; j < m; j++) if (p[j].note == a->last_note && p[j].ch == a->last_ch) { k = j; break; }
    if (k < 0 || m < 2) return (int)(rnd(a) % (uint32_t)m);
    i = (int)(rnd(a) % (uint32_t)(m - 1));
    return i >= k ? i + 1 : i;
  }
  default:   /* ARP_UP */
    i = a->has_last ? above(a, p, m) : -1;
    return i >= 0 ? i : 0;
  }
}

static void note_off_at(ArpCore* a, double tick, arp_emit_fn emit, void* user) {
  emit(user, tick, (uint8_t)(0x80 | a->on_ch), a->on_note, 0);
  a->sounding = 0;
  a->tied = 0;
}

void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user) {
  if (a->sounding) note_off_at(a, tick, emit, user);
}

/* One step of the grid at tick s. */
static void do_step(ArpCore* a, double s, arp_emit_fn emit, void* user) {
  if (a->nheld == 0) {
    if (a->sounding && a->tied) note_off_at(a, s, emit, user);   /* a tie has nothing to go to */
    a->step_count = 0;
    return;
  }
  ArpNote pat[ARP_MAX_PAT];
  int m = build_pattern(a, pat, a->order != ARP_PLAYED);
  if (m == 0) return;
  const ArpNote* n = &pat[pick(a, pat, m)];              /* the note order advances even if the step is silent */
  a->has_last = 1; a->last_ch = n->ch; a->last_note = n->note;

  /* each row reads its own step */
  const int prob  = a->steps[a->count % (uint32_t)a->lane_len[ARP_LANE_PROB]].prob;
  const int shift = a->steps[a->count % (uint32_t)a->lane_len[ARP_LANE_SHIFT]].shift;
  const int velp  = a->steps[a->count % (uint32_t)a->lane_len[ARP_LANE_VEL]].vel;
  const int gate  = a->steps[a->count % (uint32_t)a->lane_len[ARP_LANE_GATE]].gate;
  a->count++;
  a->step_count = a->count;

  int play = prob >= 100 || (prob > 0 && (int)(rnd(a) % 100u) < prob);
  int pitch = n->note + shift;
  if (pitch < 0 || pitch > 127) play = 0;
  if (!play) {
    if (a->sounding) note_off_at(a, s, emit, user);
    return;
  }
  int vel = n->vel * velp / 100;
  vel = clampi(vel, 1, 127);

  if (a->sounding && a->tied && a->on_note == pitch && a->on_ch == n->ch) {
    /* tied into the same pitch: the note just continues */
  } else if (a->sounding && a->tied) {
    uint8_t och = a->on_ch, onote = a->on_note;          /* legato: new note on before the old one off */
    emit(user, s, (uint8_t)(0x90 | n->ch), (uint8_t)pitch, (uint8_t)vel);
    emit(user, s, (uint8_t)(0x80 | och), onote, 0);
  } else {
    if (a->sounding) note_off_at(a, s, emit, user);
    emit(user, s, (uint8_t)(0x90 | n->ch), (uint8_t)pitch, (uint8_t)vel);
  }
  a->sounding = 1; a->on_ch = n->ch; a->on_note = (uint8_t)pitch;
  if (gate == ARP_GATE_TIE) {
    a->tied = 1;
    a->off_tick = INFINITY;                             /* ends at the next step */
  } else {
    int g = (gate == ARP_GATE_GLOBAL) ? a->gate_ticks : a->step_ticks * gate / 100;
    a->tied = 0;
    a->off_tick = s + (g < 1 ? 1 : g);
  }
}

void arp_process(ArpCore* a, double from, double to, arp_emit_fn emit, void* user) {
  if (to <= from) return;
  const double step = a->step_ticks;
  double s = ceil(from / step) * step;
  for (;;) {
    int off_due = a->sounding && a->off_tick < to;
    int step_due = s < to;
    if (off_due && (!step_due || a->off_tick <= s)) {
      note_off_at(a, a->off_tick > from ? a->off_tick : from, emit, user);
      continue;
    }
    if (!step_due) break;
    do_step(a, s, emit, user);
    s += step;
  }
}
