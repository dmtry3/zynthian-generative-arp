/* Generative Arp: LV2 wrapper around arp_core.
 * Transport rolling: ticks follow time:Position (steps on the song grid).
 * Transport stopped: ticks come from an internal clock at the host tempo. */
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "arp_core.h"

#define ARP_URI "urn:generativearp:arp"
#define JUMP_TICKS 4.0   /* position difference treated as a relocation */

enum { P_MIDI_IN = 0, P_MIDI_OUT = 1, P_RATE, P_GATE, P_ORDER, P_OCTAVES, P_LATCH, P_END };
#define N_CTL (P_END - P_RATE)

typedef struct { LV2_Atom_Event ev; uint8_t msg[3]; } MidiOut;

typedef struct {
  const LV2_Atom_Sequence* in;
  LV2_Atom_Sequence* out;
  const float* ctl[N_CTL];
  uint32_t out_cap, nframes, cur_frame;
  LV2_URID_Map* map;
  LV2_URID midi_Event, atom_Object, atom_Float, atom_Double, atom_Long, atom_Int;
  LV2_URID time_Position, time_bar, time_barBeat, time_bpm, time_speed, time_bpb;

  double sr, clk, bpm;   /* clk: current position in ticks */
  int bpb, rolling;
  ArpCore core;
} Arp;

static double tps(const Arp* A) { return A->bpm * ARP_PPQN / (60.0 * A->sr); }

static void emit(Arp* A, uint32_t frame, uint8_t st, uint8_t d1, uint8_t d2) {
  MidiOut e;
  memset(&e, 0, sizeof e);
  if (frame >= A->nframes) frame = A->nframes ? A->nframes - 1 : 0;
  e.ev.time.frames = frame;
  e.ev.body.type = A->midi_Event;
  e.ev.body.size = 3;
  e.msg[0] = st; e.msg[1] = d1; e.msg[2] = d2;
  lv2_atom_sequence_append_event(A->out, A->out_cap, &e.ev);
}

/* arp_core callback: convert tick to a frame of the current block */
static void core_emit(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2) {
  Arp* A = (Arp*)user;
  double d = (tick - A->clk) / tps(A);
  uint32_t f = A->cur_frame + (d > 1e-6 ? (uint32_t)ceil(d - 1e-6) : 0);
  emit(A, f, st, d1, d2);
}

static void advance_to(Arp* A, uint32_t target) {
  if (target <= A->cur_frame) return;
  double end = A->clk + (double)(target - A->cur_frame) * tps(A);
  arp_process(&A->core, A->clk, end, core_emit, A);
  A->clk = end;
  A->cur_frame = target;
}

static double numv(const Arp* A, const LV2_Atom* a, double def) {
  if (!a) return def;
  if (a->type == A->atom_Float)  return ((const LV2_Atom_Float*)a)->body;
  if (a->type == A->atom_Double) return ((const LV2_Atom_Double*)a)->body;
  if (a->type == A->atom_Long)   return (double)((const LV2_Atom_Long*)a)->body;
  if (a->type == A->atom_Int)    return ((const LV2_Atom_Int*)a)->body;
  return def;
}

static void handle_pos(Arp* A, const LV2_Atom_Object* obj) {
  const LV2_Atom *bpm = NULL, *speed = NULL, *bb = NULL, *bar = NULL, *bpb = NULL;
  lv2_atom_object_get(obj, A->time_bpm, &bpm, A->time_speed, &speed, A->time_barBeat, &bb,
                      A->time_bar, &bar, A->time_bpb, &bpb, 0);
  if (bpm) { double v = numv(A, bpm, A->bpm); if (v >= 20 && v <= 400) A->bpm = v; }
  if (bpb) { int v = (int)(numv(A, bpb, 4) + 0.5); if (v >= 1 && v <= 16) A->bpb = v; }
  if (speed) A->rolling = numv(A, speed, 0) != 0;
  /* Stopped: the position is frozen, keep the internal clock running. */
  if (!A->rolling || !bar || !bb) return;
  double ht = (numv(A, bar, 0) * A->bpb + numv(A, bb, 0)) * ARP_PPQN;
  if (fabs(ht - A->clk) > JUMP_TICKS) arp_silence(&A->core, A->clk, core_emit, A);
  A->clk = ht;
}

static void handle_midi(Arp* A, const LV2_Atom_Event* ev) {
  const uint8_t* b = (const uint8_t*)LV2_ATOM_BODY_CONST(&ev->body);
  uint32_t sz = ev->body.size;
  if (sz < 1) return;
  uint8_t type = sz >= 3 ? (b[0] & 0xF0) : 0, ch = b[0] & 0x0F;
  if (type == 0x90 && b[2] > 0) {
    int first = arp_note_on(&A->core, ch, b[1], b[2]);
    if (first && !A->rolling) {
      /* Free-running: start on a step right now instead of waiting for the next one. */
      const double step = A->core.step_ticks;
      A->clk = ceil(A->clk / step) * step;
    }
    return;
  }
  if (type == 0x80 || type == 0x90) { arp_note_off(&A->core, ch, b[1]); return; }
  if (type == 0xB0 && (b[1] == 123 || b[1] == 120)) arp_release_all(&A->core);
  lv2_atom_sequence_append_event(A->out, A->out_cap, ev);   /* everything else passes through */
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* path,
                              const LV2_Feature* const* f) {
  (void)d; (void)path;
  Arp* A = (Arp*)calloc(1, sizeof(Arp));
  if (!A) return NULL;
  for (int i = 0; f[i]; i++)
    if (!strcmp(f[i]->URI, LV2_URID__map)) A->map = (LV2_URID_Map*)f[i]->data;
  if (!A->map) { free(A); return NULL; }
  LV2_URID_Map* m = A->map;
  A->midi_Event    = m->map(m->handle, LV2_MIDI__MidiEvent);
  A->atom_Object   = m->map(m->handle, LV2_ATOM__Object);
  A->atom_Float    = m->map(m->handle, LV2_ATOM__Float);
  A->atom_Double   = m->map(m->handle, LV2_ATOM__Double);
  A->atom_Long     = m->map(m->handle, LV2_ATOM__Long);
  A->atom_Int      = m->map(m->handle, LV2_ATOM__Int);
  A->time_Position = m->map(m->handle, LV2_TIME__Position);
  A->time_bar      = m->map(m->handle, LV2_TIME__bar);
  A->time_barBeat  = m->map(m->handle, LV2_TIME__barBeat);
  A->time_bpm      = m->map(m->handle, LV2_TIME__beatsPerMinute);
  A->time_speed    = m->map(m->handle, LV2_TIME__speed);
  A->time_bpb      = m->map(m->handle, LV2_TIME__beatsPerBar);
  A->sr = rate; A->bpm = 120.0; A->bpb = 4;
  arp_init(&A->core);
  arp_seed(&A->core, (uint32_t)(uintptr_t)A ^ 0xA5A5A5A5u);
  return A;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
  Arp* A = (Arp*)h;
  if (port == P_MIDI_IN) A->in = (const LV2_Atom_Sequence*)data;
  else if (port == P_MIDI_OUT) A->out = (LV2_Atom_Sequence*)data;
  else if (port >= P_RATE && port < P_END) A->ctl[port - P_RATE] = (const float*)data;
}

static int ctl_int(const Arp* A, int port, int def) {
  const float* p = A->ctl[port - P_RATE];
  return p ? (int)lrintf(*p) : def;
}

static void activate(LV2_Handle h) { (void)h; }
static void deactivate(LV2_Handle h) { (void)h; }

static void run(LV2_Handle h, uint32_t n) {
  Arp* A = (Arp*)h;
  A->out_cap = A->out->atom.size;
  lv2_atom_sequence_clear(A->out);
  A->out->atom.type = A->in->atom.type;
  A->nframes = n; A->cur_frame = 0;
  arp_set_params(&A->core, ctl_int(A, P_RATE, ARP_RATE_16), ctl_int(A, P_GATE, 50),
                 ctl_int(A, P_ORDER, ARP_UP), ctl_int(A, P_OCTAVES, 1));
  arp_set_latch(&A->core, ctl_int(A, P_LATCH, 0) != 0);

  LV2_ATOM_SEQUENCE_FOREACH(A->in, ev) {
    uint32_t f = (uint32_t)ev->time.frames;
    if (f >= n) f = n ? n - 1 : 0;
    if (f < A->cur_frame) f = A->cur_frame;
    advance_to(A, f);
    if (ev->body.type == A->midi_Event) handle_midi(A, ev);
    else if (ev->body.type == A->atom_Object) {
      const LV2_Atom_Object* o = (const LV2_Atom_Object*)&ev->body;
      if (o->body.otype == A->time_Position) handle_pos(A, o);
    }
  }
  advance_to(A, n);
}

static void cleanup(LV2_Handle h) { free(h); }
static const void* extension_data(const char* uri) { (void)uri; return NULL; }

static const LV2_Descriptor desc = { ARP_URI, instantiate, connect_port, activate,
                                     run, deactivate, cleanup, extension_data };
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t i) { return i == 0 ? &desc : NULL; }
