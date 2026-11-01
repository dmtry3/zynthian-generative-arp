/* Arpeggiator logic without LV2 dependencies. Time is measured in ticks (ARP_PPQN per beat). */
#ifndef ARP_CORE_H
#define ARP_CORE_H

#include <stdint.h>

#define ARP_PPQN 960
#define ARP_MAX_NOTES 32
#define ARP_MAX_OCTAVES 4
#define ARP_MAX_STEPS 16
#define ARP_GATE_GLOBAL 0     /* step gate: use the global Gate */
#define ARP_GATE_TIE 101      /* step gate: legato into the next step */

enum { ARP_RATE_4, ARP_RATE_8, ARP_RATE_8T, ARP_RATE_16, ARP_RATE_16T, ARP_RATE_32, ARP_NRATES };
enum { ARP_UP, ARP_DOWN, ARP_UPDOWN, ARP_DOWNUP, ARP_PLAYED, ARP_RANDOM, ARP_NORDERS };

/* Called for every outgoing MIDI event; tick is inside the range passed to arp_process(). */
typedef void (*arp_emit_fn)(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2);

typedef struct { uint8_t ch, note, vel; uint32_t seq; } ArpNote;   /* seq: press order */

/* Per-step settings of the pattern. */
typedef struct {
  int prob;    /* 0..100 %: chance that the step plays */
  int shift;   /* -24..+24 semitones added to the chosen note */
  int vel;     /* 0..200 % of the key velocity (result clamped to 1..127) */
  int gate;    /* ARP_GATE_GLOBAL, 5..100 % of the step, or ARP_GATE_TIE */
} ArpStep;

typedef struct {
  ArpNote held[ARP_MAX_NOTES];  /* notes being arpeggiated, sorted by note, then channel */
  int nheld;
  uint32_t press_seq;
  ArpNote phys[ARP_MAX_NOTES];  /* keys physically down (unsorted) */
  int nphys;
  int latch;                    /* latched: released keys stay in held until a new chord */

  /* parameters */
  int rate, gate_pct, order, octaves;
  int step_ticks, gate_ticks;
  ArpStep steps[ARP_MAX_STEPS];
  int length;                   /* pattern length 1..16 */

  /* note order state; reset when all keys are released */
  int has_last;                 /* last played pitch valid */
  uint8_t last_ch, last_note;
  int dir;                      /* +1 / -1 for Up-Down and Down-Up */
  uint32_t played_idx;          /* As played position */
  int step_pos;                 /* next pattern step */
  int cur_step;                 /* step of the last grid step, 1..16; 0 when idle (for display) */
  uint32_t rng;

  int sounding;                 /* a generated note is on */
  int tied;                     /* the sounding note is tied into the next step */
  uint8_t on_ch, on_note;
  double off_tick;
} ArpCore;

void arp_init(ArpCore* a);
/* Values are clamped. Rate/order take effect on the next step, gate on the next note. */
void arp_set_params(ArpCore* a, int rate, int gate_pct, int order, int octaves);
void arp_seed(ArpCore* a, uint32_t seed);
/* Pattern: length 1..16 and settings of step i (0-based). Values are clamped. */
void arp_set_length(ArpCore* a, int length);
void arp_set_step(ArpCore* a, int i, int prob, int shift, int vel, int gate);
/* Latch on: the chord stays after the keys are released; the next chord (pressed after all keys
 * were released) replaces it. Latch off: only the keys still down are kept. */
void arp_set_latch(ArpCore* a, int on);
/* Returns 1 if nothing was being arpeggiated before this key. */
int  arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel);
void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note);
/* All notes off: clears pressed keys and the latched chord. */
void arp_release_all(ArpCore* a);
/* Generate events with tick in [from, to). Steps fall on multiples of step_ticks. */
void arp_process(ArpCore* a, double from, double to, arp_emit_fn emit, void* user);
/* Turn off the generated note now (transport jump etc.). */
void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user);

#endif
