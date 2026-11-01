/* Arpeggiator logic without LV2 dependencies. Time is measured in ticks (ARP_PPQN per beat). */
#ifndef ARP_CORE_H
#define ARP_CORE_H

#include <stdint.h>

#define ARP_PPQN 960
#define ARP_MAX_NOTES 32

/* Called for every outgoing MIDI event; tick is inside the range passed to arp_process(). */
typedef void (*arp_emit_fn)(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2);

typedef struct { uint8_t ch, note, vel; } ArpNote;

typedef struct {
  ArpNote held[ARP_MAX_NOTES];  /* pressed keys, sorted by note, then channel */
  int nheld;
  int step_ticks, gate_ticks;
  int has_last;                 /* last played key valid; cleared when all keys are released */
  uint8_t last_ch, last_note;
  int sounding;                 /* a generated note is on */
  uint8_t on_ch, on_note;
  double off_tick;
} ArpCore;

void arp_init(ArpCore* a);
/* Returns 1 if this was the first pressed key (nothing was held before). */
int  arp_note_on(ArpCore* a, uint8_t ch, uint8_t note, uint8_t vel);
void arp_note_off(ArpCore* a, uint8_t ch, uint8_t note);
void arp_release_all(ArpCore* a);
/* Generate events with tick in [from, to). Steps fall on multiples of step_ticks. */
void arp_process(ArpCore* a, double from, double to, arp_emit_fn emit, void* user);
/* Turn off the generated note now (transport jump etc.). */
void arp_silence(ArpCore* a, double tick, arp_emit_fn emit, void* user);

#endif
