/* Arpeggiator logic without LV2 dependencies. Time is measured in ticks (ARP_PPQN per beat). */
#ifndef ARP_CORE_H
#define ARP_CORE_H

#include <stdint.h>

#define ARP_PPQN 960
#define ARP_MAX_NOTES 32
#define ARP_MAX_OCTAVES 4

enum { ARP_RATE_4, ARP_RATE_8, ARP_RATE_8T, ARP_RATE_16, ARP_RATE_16T, ARP_RATE_32, ARP_NRATES };
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_PLAYED, ARP_RANDOM, ARP_NORDERS };

/* Called for every outgoing MIDI event; tick is inside the range passed to arp_process(). */
typedef void (*arp_emit_fn)(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2);

typedef struct { uint8_t ch, note, vel; uint32_t seq; } ArpNote;   /* seq: press order */

typedef struct {
  ArpNote held[ARP_MAX_NOTES];  /* pressed keys, sorted by note, then channel */
  int nheld;
  uint32_t press_seq;

  /* parameters */
  int rate, gate_pct, order, octaves;
  int step_ticks, gate_ticks;

  /* note order state; reset when all keys are released */
  int has_last;                 /* last played pitch valid */
  uint8_t last_ch, last_note;
  int dir;                      /* +1 / -1 for Up-Down and Down-Up */
  uint32_t played_idx;          /* As played position */
  uint32_t rng;

  int sounding;                 /* a generated note is on */
  uint8_t on_ch, on_note;
  double off_tick;
} ArpCore;

void arp_init(ArpCore* a);
/* Values are clamped. Rate/order take effect on the next step, gate on the next note. */
void arp_set_params(ArpCore* a, int rate, int gate_pct, int order, int octaves);
void arp_seed(ArpCore* a, uint32_t seed);
/* Returns 1 if this was the first pressed key (nothing was held before). */
int  arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel);
void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note);
void arp_release_all(ArpCore* a);
/* Generate events with tick in [from, to). Steps fall on multiples of step_ticks. */
void arp_process(ArpCore* a, double from, double to, arp_emit_fn emit, void* user);
/* Turn off the generated note now (transport jump etc.). */
void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user);

#endif
