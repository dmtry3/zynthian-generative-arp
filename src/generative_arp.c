/* Generative Arp, stage 1: empty LV2 MIDI plugin that passes MIDI through unchanged. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ARP_URI "urn:generativearp:arp"

enum { P_MIDI_IN = 0, P_MIDI_OUT = 1 };

typedef struct {
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* out;
  LV2_URID_Map* map;
  LV2_URID midi_Event;
} Arp;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path,
                              const LV2_Feature* const* f) {
  (void)d; (void)rate; (void)path;
  Arp* A = (Arp*)calloc(1, sizeof(Arp));
  if (!A) return NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) A->map = (LV2_URID_Map*)f[i]->data;
  if (!A->map) { free(A); return NULL; }
  A->midi_Event = A->map->map(A->map->handle, LV2_MIDI__MidiEvent);
  return A;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Arp* A = (Arp*)h;
  if (port == P_MIDI_IN) A->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_MIDI_OUT) A->out = (LV2_Atom_Sequence*)data;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

static void run(LV2_Handle h, uint32_t n) {
  (void)n;
  Arp* A = (Arp*)h;
  const uint32_t cap = A->out->atom.size;
  lv2_atom_sequence_clear(A->out);
  A->out->atom.type = A->in->atom.type;

  LV2_ATOM_SEQUENCE_FOREACH(A->in, ev) {
    if (ev->body.type == A->midi_Event)
      lv2_atom_sequence_append_event(A->out, cap, ev);
  }
}

static void cleanup(LV2_Handle h) { free(h); }
static const void* extension_data(const char* uri) { (void)uri; return NULL; }

static const LV2_Descriptor desc = { ARP_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
