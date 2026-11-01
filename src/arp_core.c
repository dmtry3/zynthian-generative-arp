#include "arp_core.h"
#include <math.h>
#include <string.h>

void arp_init(ArpCore* a) {
  memset(a, 0, sizeof *a);
  a->step_ticks = ARP_PPQN / 4;   /* 1/16 */
  a->gate_ticks = ARP_PPQN / 8;   /* 50% of the step */
}

static int key_less(uint8_t n1, uint8_t c1, uint8_t n2, uint8_t c2) {
  return n1 != n2 ? n1 < n2 : c1 < c2;
}

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
  if (a->nheld == 0) a->has_last = 0;
}

void arp_release_all(ArpCore* a) { a->nheld = 0; a->has_last = 0; }

/* Up: the lowest held key above the last played one, wrapping to the lowest. */
static const ArpNote* next_up(const ArpCore* a) {
  if (a->has_last)
    for (int i = 0; i < a->nheld; i++)
      if (key_less(a->last_note, a->last_ch, a->held[i].note, a->held[i].ch)) return &a->held[i];
  return &a->held[0];
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
      if (a->sounding) note_off_at(a, s, emit, user);
      const ArpNote* n = next_up(a);
      a->has_last = 1; a->last_ch = n->ch; a->last_note = n->note;
      emit(user, s, (uint8_t)(0x90 | n->ch), n->note, n->vel);
      a->sounding = 1; a->on_ch = n->ch; a->on_note = n->note;
      a->off_tick = s + a->gate_ticks;
    }
    s += step;
  }
}
