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

static void reset_order(ArpCore* a) { a->has_last = 0; a->played_idx = 0; }

int arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel) {
  int first = (a->nheld == 0);
  for (int i = 0; i < a->nheld; i++)
    if (a->held[i].ch == ch && a->held[i].note == note) { a->held[i].vel = vel; return first; }
  if (a->nheld >= ARP_MAX_NOTES) return first;
  int pos = a->nheld;
  while (pos > 0 && key_less(note, ch, a->held[pos - 1].note, a->held[pos - 1].ch)) {
    a->held[pos] = a->held[pos - 1];
    pos--;
  }
  a->held[pos].ch = ch; a->held[pos].note = note; a->held[pos].vel = vel;
  a->held[pos].seq = a->press_seq++;
  a->nheld++;
  return first;
}

void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note) {
  for (int i = 0; i < a->nheld; i++) {
    if (a->held[i].ch == ch && a->held[i].note == note) {
      memmove(&a->held[i], &a->held[i + 1], (size_t)(a->nheld - i - 1) * sizeof(ArpNote));
      a->nheld--;
      break;
    }
  }
  if (a->nheld == 0) reset_order(a);
}

void arp_release_all(ArpCore* a) { a->nheld = 0; reset_order(a); }

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
}

void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user) {
  if (a->sounding) note_off_at(a, tick, emit, user);
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
    if (a->nheld > 0) {
      ArpNote pat[ARP_MAX_PAT];
      int m = build_pattern(a, pat, a->order != ARP_PLAYED);
      if (m > 0) {
        if (a->sounding) note_off_at(a, s, emit, user);
        const ArpNote* n = &pat[pick(a, pat, m)];
        a->has_last = 1; a->last_ch = n->ch; a->last_note = n->note;
        emit(user, s, (uint8_t)(0x90 | n->ch), n->note, n->vel);
        a->sounding = 1; a->on_ch = n->ch; a->on_note = n->note;
        a->off_tick = s + a->gate_ticks;
      }
    }
    s += step;
  }
}
