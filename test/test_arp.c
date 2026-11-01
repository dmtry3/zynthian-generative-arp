/* Tests for arp_core without LV2: feed notes and ticks, print and check outgoing events. */
#include "../src/arp_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct { double tick; uint8_t st, d1, d2; } Ev;
typedef struct { Ev ev[512]; int n; } Log;

static int failures = 0;

static void collect(void* user, double tick, uint8_t st, uint8_t d1, uint8_t d2) {
  Log* l = (Log*)user;
  if (l->n < 512) { l->ev[l->n].tick = tick; l->ev[l->n].st = st; l->ev[l->n].d1 = d1; l->ev[l->n].d2 = d2; l->n++; }
}

static void print_log(const char* title, const Log* l) {
  printf("  %s (%d events):\n", title, l->n);
  for (int i = 0; i < l->n; i++)
    printf("    %7.1f  %s ch%d note %3d vel %3d\n", l->ev[i].tick,
           (l->ev[i].st & 0xF0) == 0x90 ? "ON " : "OFF", l->ev[i].st & 0x0F, l->ev[i].d1, l->ev[i].d2);
}

/* expected: triples {tick, on(1)/off(0), note} */
static void check(const char* name, const Log* l, const double (*exp)[3], int nexp) {
  int ok = (l->n == nexp);
  for (int i = 0; ok && i < nexp; i++) {
    int on = (l->ev[i].st & 0xF0) == 0x90;
    if (fabs(l->ev[i].tick - exp[i][0]) > 1e-6 || on != (int)exp[i][1] || l->ev[i].d1 != (uint8_t)exp[i][2]) ok = 0;
  }
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) { print_log("got", l); failures++; }
}

static int same_logs(const Log* a, const Log* b) {
  if (a->n != b->n) return 0;
  for (int i = 0; i < a->n; i++)
    if (fabs(a->ev[i].tick - b->ev[i].tick) > 1e-6 || a->ev[i].st != b->ev[i].st || a->ev[i].d1 != b->ev[i].d1 ||
        a->ev[i].d2 != b->ev[i].d2) return 0;
  return 1;
}

static void chord(ArpCore* a) {
  arp_note_on(a, 0, 64, 90);
  arp_note_on(a, 0, 60, 100);   /* pressed out of order: arp must still go up */
  arp_note_on(a, 0, 67, 80);
}

static const double EXP_UP[][3] = {
  {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {360, 0, 64}, {480, 1, 67}, {600, 0, 67},
  {720, 1, 60}, {840, 0, 60}, {960, 1, 64}, {1080, 0, 64}, {1200, 1, 67}, {1320, 0, 67},
  {1440, 1, 60}, {1560, 0, 60}, {1680, 1, 64}, {1800, 0, 64},
};

static void test_up_one_block(void) {
  ArpCore a; Log l = {0};
  arp_init(&a); chord(&a);
  arp_process(&a, 0, 1920, collect, &l);
  check("chord C-E-G, up, 1/16, gate 50%, one block", &l, EXP_UP, 16);
  if (l.n && l.ev[0].d2 == 100 && l.ev[2].d2 == 90 && l.ev[4].d2 == 80 && l.ev[0].st == 0x90)
    printf("PASS: velocity and channel taken from held keys\n");
  else { printf("FAIL: velocity and channel taken from held keys\n"); failures++; }
}

static void run_chunks(Log* l, const double* sizes, int nsizes) {
  ArpCore a; arp_init(&a); chord(&a);
  double t = 0; int i = 0;
  while (t < 1920) {
    double e = t + sizes[i++ % nsizes];
    if (e > 1920) e = 1920;
    arp_process(&a, t, e, collect, l);
    t = e;
  }
}

static void test_chunks(void) {
  Log ref = {0};
  ArpCore a; arp_init(&a); chord(&a);
  arp_process(&a, 0, 1920, collect, &ref);
  const double s1[] = { 1 }, s2[] = { 7.3 }, s3[] = { 240 }, s4[] = { 120, 0.5, 333.3, 239.9, 0.1, 64 };
  const double* sets[] = { s1, s2, s3, s4 };
  const int lens[] = { 1, 1, 1, 6 };
  const char* names[] = { "1 tick", "7.3 ticks", "240 ticks (step on boundary)", "mixed sizes" };
  for (int k = 0; k < 4; k++) {
    Log l = {0};
    run_chunks(&l, sets[k], lens[k]);
    char name[96]; snprintf(name, sizeof name, "chunks of %s give the same events", names[k]);
    printf("%s: %s\n", same_logs(&ref, &l) ? "PASS" : "FAIL", name);
    if (!same_logs(&ref, &l)) { print_log("got", &l); failures++; }
  }
}

static void test_add_release(void) {
  ArpCore a; Log l = {0};
  arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_note_on(&a, 0, 64, 100);
  arp_process(&a, 0, 300, collect, &l);          /* 60@0, 64@240 */
  arp_note_on(&a, 0, 62, 100);                   /* now 60 62 64; last was 64 (top) -> wraps to 60 */
  arp_process(&a, 300, 700, collect, &l);        /* off 64@360, on 60@480, off@600 */
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 62); arp_note_off(&a, 0, 64);
  arp_process(&a, 700, 1500, collect, &l);       /* nothing */
  int first = arp_note_on(&a, 0, 67, 100);
  arp_note_on(&a, 0, 65, 100);
  arp_process(&a, 1500, 2000, collect, &l);      /* restarts from lowest: 65@1680, 67@1920 */
  static const double exp[][3] = {
    {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {360, 0, 64}, {480, 1, 60}, {600, 0, 60},
    {1680, 1, 65}, {1800, 0, 65}, {1920, 1, 67},
  };
  check("add note mid-chord, release all, new chord starts from lowest", &l, exp, 9);
  printf("%s: arp_note_on reports first key\n", first ? "PASS" : "FAIL");
  if (!first) failures++;
}

static void test_insert_below(void) {
  ArpCore a; Log l = {0};
  arp_init(&a); chord(&a);                       /* 60 64 67 */
  arp_process(&a, 0, 300, collect, &l);          /* 60@0, 64@240 */
  arp_note_on(&a, 0, 62, 100);                   /* inserted below the last played: must go on to 67 */
  arp_note_off(&a, 0, 60);                       /* removing a key does not break the order */
  arp_process(&a, 300, 1000, collect, &l);       /* 67@480, wrap to lowest 62@720, 64@960 */
  static const double exp[][3] = {
    {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {360, 0, 64}, {480, 1, 67}, {600, 0, 67},
    {720, 1, 62}, {840, 0, 62}, {960, 1, 64},
  };
  check("keys added/removed mid-arp: order keeps going up from last note", &l, exp, 9);
}

static void test_silence(void) {
  ArpCore a; Log l = {0};
  arp_init(&a);
  arp_note_on(&a, 2, 60, 100);
  arp_process(&a, 0, 50, collect, &l);           /* on@0 */
  arp_silence(&a, 50, collect, &l);              /* off@50 */
  arp_silence(&a, 51, collect, &l);              /* nothing */
  arp_process(&a, 50, 200, collect, &l);         /* no second off at 120 */
  static const double exp[][3] = { {0, 1, 60}, {50, 0, 60} };
  check("silence turns note off once, no double note-off", &l, exp, 2);
  if (l.n == 2 && l.ev[1].st == 0x82) printf("PASS: note-off on the note's channel\n");
  else { printf("FAIL: note-off on the note's channel\n"); failures++; }
}

static void test_late_off(void) {
  ArpCore a; Log l = {0};
  arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_process(&a, 0, 10, collect, &l);           /* on@0, off due at 120 */
  arp_note_off(&a, 0, 60);
  arp_process(&a, 200, 230, collect, &l);        /* clock jumped past the off: emit at 200 */
  static const double exp[][3] = { {0, 1, 60}, {200, 0, 60} };
  check("overdue note-off is sent at block start", &l, exp, 2);
}

static void test_release_all(void) {
  ArpCore a; Log l = {0};
  arp_init(&a); chord(&a);
  arp_process(&a, 0, 100, collect, &l);
  arp_release_all(&a);
  arp_process(&a, 100, 1000, collect, &l);
  static const double exp[][3] = { {0, 1, 60}, {120, 0, 60} };
  check("release all: current note ends by gate, nothing new", &l, exp, 2);
}

/* ---------- stage 3: parameters ---------- */

/* note-on pitches of a log, in order */
static int ons(const Log* l, int* out) {
  int k = 0;
  for (int i = 0; i < l->n; i++) if ((l->ev[i].st & 0xF0) == 0x90) out[k++] = l->ev[i].d1;
  return k;
}

static void check_seq(const char* name, const Log* l, const int* exp, int nexp) {
  int got[512]; int n = ons(l, got);
  int ok = (n == nexp);
  for (int i = 0; ok && i < n; i++) if (got[i] != exp[i]) ok = 0;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    printf("  got:"); for (int i = 0; i < n; i++) printf(" %d", got[i]);
    printf("\n  exp:"); for (int i = 0; i < nexp; i++) printf(" %d", exp[i]);
    printf("\n"); failures++;
  }
}

/* chord C-E-G, given order/octaves, 1/16, n steps */
static void run_order(Log* l, int order, int octaves, int nsteps) {
  ArpCore a; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 50, order, octaves);
  arp_process(&a, 0, 240.0 * nsteps, collect, l);
}

static void test_orders(void) {
  struct { const char* name; int order, oct, n; int exp[16]; } t[] = {
    { "Up, 1 oct",        ARP_UP,     1, 7, { 60, 64, 67, 60, 64, 67, 60 } },
    { "Down, 1 oct",      ARP_DOWN,   1, 7, { 67, 64, 60, 67, 64, 60, 67 } },
    { "Up-Down, no repeated ends", ARP_UPDOWN, 1, 9, { 60, 64, 67, 64, 60, 64, 67, 64, 60 } },
    { "Down-Up, no repeated ends", ARP_DOWNUP, 1, 9, { 67, 64, 60, 64, 67, 64, 60, 64, 67 } },
    { "Up, 2 oct",        ARP_UP,     2, 8, { 60, 64, 67, 72, 76, 79, 60, 64 } },
    { "Down, 2 oct",      ARP_DOWN,   2, 7, { 79, 76, 72, 67, 64, 60, 79 } },
    { "Up-Down, 2 oct",   ARP_UPDOWN, 2, 12, { 60, 64, 67, 72, 76, 79, 76, 72, 67, 64, 60, 64 } },
    { "Up, 4 oct",        ARP_UP,     4, 13, { 60, 64, 67, 72, 76, 79, 84, 88, 91, 96, 100, 103, 60 } },
  };
  for (unsigned k = 0; k < sizeof t / sizeof t[0]; k++) {
    Log l = {0}; run_order(&l, t[k].order, t[k].oct, t[k].n);
    char name[96]; snprintf(name, sizeof name, "order %s", t[k].name);
    check_seq(name, &l, t[k].exp, t[k].n);
  }
}

static void test_single_note_orders(void) {
  for (int o = 0; o < ARP_NORDERS; o++) {
    ArpCore a; Log l = {0}; arp_init(&a);
    arp_set_params(&a, ARP_RATE_16, 50, o, 1);
    arp_note_on(&a, 0, 62, 100);
    arp_process(&a, 0, 240 * 4, collect, &l);
    static const int exp[] = { 62, 62, 62, 62 };
    char name[96]; snprintf(name, sizeof name, "single key repeats with order %d", o);
    check_seq(name, &l, exp, 4);
  }
}

static void test_played(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_PLAYED, 2);
  arp_note_on(&a, 0, 64, 100); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 67, 100);
  arp_process(&a, 0, 240 * 7, collect, &l);
  static const int exp[] = { 64, 60, 67, 76, 72, 79, 64 };
  check_seq("As played (E C G), 2 oct", &l, exp, 7);
}

static void random_run(Log* l, uint32_t seed) {
  ArpCore a; arp_init(&a); chord(&a);
  arp_seed(&a, seed);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_RANDOM, 2);
  arp_process(&a, 0, 240 * 200, collect, l);
}

static void test_random(void) {
  Log l1 = {0}, l2 = {0}, l3 = {0};
  random_run(&l1, 42); random_run(&l2, 42); random_run(&l3, 7);
  int s1[512], s3[512]; int n = ons(&l1, s1); ons(&l3, s3);
  int ok_set = (n == 200), ok_rep = 1, hits[128] = {0};
  for (int i = 0; i < n; i++) {
    int p = s1[i];
    if (p != 60 && p != 64 && p != 67 && p != 72 && p != 76 && p != 79) ok_set = 0;
    else hits[p]++;
    if (i > 0 && s1[i] == s1[i - 1]) ok_rep = 0;
  }
  int all = hits[60] && hits[64] && hits[67] && hits[72] && hits[76] && hits[79];
  printf("%s: Random uses only held notes (2 oct), all of them\n", ok_set && all ? "PASS" : "FAIL");
  printf("%s: Random never repeats a note twice in a row\n", ok_rep ? "PASS" : "FAIL");
  printf("%s: Random with the same seed is reproducible\n", same_logs(&l1, &l2) ? "PASS" : "FAIL");
  printf("%s: Random with another seed differs\n", memcmp(s1, s3, sizeof(int) * 200) ? "PASS" : "FAIL");
  failures += !(ok_set && all) + !ok_rep + !same_logs(&l1, &l2) + !memcmp(s1, s3, sizeof(int) * 200);
}

static void test_rates(void) {
  static const int step[ARP_NRATES] = { 960, 480, 320, 240, 160, 120 };
  static const char* names[ARP_NRATES] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };
  for (int r = 0; r < ARP_NRATES; r++) {
    ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
    arp_set_params(&a, r, 50, ARP_UP, 1);
    arp_process(&a, 100, 100 + 3840, collect, &l);     /* start off-grid */
    int ok = 1, k = 0;
    for (int i = 0; i < l.n; i++) {
      double t = l.ev[i].tick;
      if ((l.ev[i].st & 0xF0) == 0x90) {
        if (fmod(t, step[r]) != 0) ok = 0;
        k++;
      } else if (fmod(t - step[r] / 2, step[r]) != 0) ok = 0;
    }
    int expn = (int)(floor((3940.0 - 1) / step[r]) - floor(99.0 / step[r]));   /* grid points in [100, 3940) */
    char name[96]; snprintf(name, sizeof name, "rate %s: notes on its grid, gate half a step", names[r]);
    printf("%s: %s\n", ok && k == expn ? "PASS" : "FAIL", name);
    if (!(ok && k == expn)) { print_log("got", &l); failures++; }
  }
}

static void test_gates(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 5, ARP_UP, 1);
  arp_process(&a, 0, 480, collect, &l);
  static const double e5[][3] = { {0, 1, 60}, {12, 0, 60}, {240, 1, 64}, {252, 0, 64} };
  check("gate 5% of 1/16 = 12 ticks", &l, e5, 4);

  Log l2 = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 100, ARP_UP, 1);
  arp_process(&a, 0, 480, collect, &l2);
  static const double e100[][3] = { {0, 1, 60}, {240, 0, 60}, {240, 1, 64} };
  check("gate 100%: note-off before next note-on on the same tick", &l2, e100, 3);

  Log l3 = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_16, 0, 99, 9);        /* out of range values are clamped */
  printf("%s: parameters are clamped\n", a.gate_pct == 5 && a.order == ARP_RANDOM && a.octaves == 4 ? "PASS" : "FAIL");
  if (!(a.gate_pct == 5 && a.order == ARP_RANDOM && a.octaves == 4)) failures++;
  (void)l3;
}

static void test_live_changes(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_params(&a, ARP_RATE_4, 100, ARP_UP, 1);
  arp_process(&a, 0, 1000, collect, &l);             /* 60@0 off@960, 64@960 (sounding till 1920) */
  arp_set_params(&a, ARP_RATE_32, 50, ARP_DOWN, 1);   /* faster, and change order */
  arp_process(&a, 1000, 1400, collect, &l);          /* 64 cut at next 1/32 step 1080; down from 64: 60, 67, 64 */
  static const double exp[][3] = {
    {0, 1, 60}, {960, 0, 60}, {960, 1, 64},
    {1080, 0, 64}, {1080, 1, 60}, {1140, 0, 60}, {1200, 1, 67}, {1260, 0, 67}, {1320, 1, 64}, {1380, 0, 64},
  };
  check("rate 1/4 -> 1/32 and Up -> Down on the fly: new grid, order continues from last note", &l, exp, 10);
}

static void test_high_notes(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_UP, 4);
  arp_note_on(&a, 0, 100, 100);
  arp_note_on(&a, 0, 110, 100);
  arp_process(&a, 0, 240 * 5, collect, &l);
  static const int exp[] = { 100, 110, 112, 122, 124 };   /* 134, 136, 146 dropped */
  check_seq("notes above 127 are skipped", &l, exp, 5);
}

static void test_wide_chord_octaves(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_set_params(&a, ARP_RATE_16, 50, ARP_UP, 2);
  arp_note_on(&a, 0, 48, 100); arp_note_on(&a, 0, 60, 100); arp_note_on(&a, 0, 62, 100);
  arp_process(&a, 0, 240 * 6, collect, &l);
  static const int exp[] = { 48, 60, 62, 72, 74, 48 };      /* 60 from both octaves played once */
  check_seq("span over an octave: sorted, duplicates removed", &l, exp, 6);
}

/* ---------- latch ---------- */

/* note-on pitches produced over [from, to) */
static int play(ArpCore* a, double from, double to, int* out) {
  Log l = {0};
  arp_process(a, from, to, collect, &l);
  return ons(&l, out);
}

static void expect(const char* name, const int* got, int n, const int* exp, int nexp) {
  int ok = (n == nexp);
  for (int i = 0; ok && i < n; i++) if (got[i] != exp[i]) ok = 0;
  printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    printf("  got:"); for (int i = 0; i < n; i++) printf(" %d", got[i]);
    printf("\n  exp:"); for (int i = 0; i < nexp; i++) printf(" %d", exp[i]);
    printf("\n"); failures++;
  }
}

static void test_latch(void) {
  int g[64], n;
  ArpCore a;

  /* latch on: chord keeps playing after release */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 240 * 4, g);
  { static const int e[] = { 60, 64, 67, 60 }; expect("latch: chord keeps playing after keys released", g, n, e, 4); }

  /* new chord after release replaces it, order restarts */
  arp_note_on(&a, 0, 50, 100); arp_note_on(&a, 0, 53, 100);
  n = play(&a, 960, 960 + 240 * 3, g);
  { static const int e[] = { 50, 53, 50 }; expect("latch: next chord replaces latched one, order from start", g, n, e, 3); }

  /* adding a key while others are down adds to the chord; releasing one keeps it */
  arp_note_on(&a, 0, 57, 100);                 /* 50 53 still down */
  arp_note_off(&a, 0, 50);                     /* 53 57 down, 50 stays latched */
  n = play(&a, 1680, 1680 + 240 * 3, g);       /* last was 50 -> 53 57 50 */
  { static const int e[] = { 53, 57, 50 }; expect("latch: key added while holding joins, released key stays", g, n, e, 3); }

  /* latch off while some keys are down: only those stay */
  arp_set_latch(&a, 0);                        /* 53 57 down */
  n = play(&a, 2400, 2400 + 240 * 3, g);       /* last 50 -> 53 57 53 */
  { static const int e[] = { 53, 57, 53 }; expect("latch off with keys down: only pressed keys keep playing", g, n, e, 3); }

  /* latch off with nothing down: stops */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 240, g);
  arp_set_latch(&a, 0);
  Log l = {0};
  arp_process(&a, 240, 1200, collect, &l);
  { static const double e[][3] = { {120, 0, 60} };
    (void)n; check("latch off with nothing down: arp stops (sounding note already ended)", &l, e, 0); }
  printf("%s: latch off clears the latched chord\n", a.nheld == 0 ? "PASS" : "FAIL");
  if (a.nheld) failures++;

  /* latch turned on while holding: chord latches on release */
  arp_init(&a); chord(&a);
  arp_set_latch(&a, 1);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 240 * 3, g);
  { static const int e[] = { 60, 64, 67 }; expect("latch on while holding: chord latches on release", g, n, e, 3); }

  /* latch on with nothing held: nothing happens until a chord */
  arp_init(&a); arp_set_latch(&a, 1);
  n = play(&a, 0, 960, g);
  { static const int e[] = { 0 }; expect("latch on with nothing pressed: silence", g, n, e, 0); }

  /* all notes off clears the latched chord */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  arp_release_all(&a);
  n = play(&a, 0, 960, g);
  { static const int e[] = { 0 }; expect("all notes off clears the latched chord", g, n, e, 0); }
  /* and the next press starts fresh (first key) */
  int first = arp_note_on(&a, 0, 62, 100);
  printf("%s: after all notes off the next key is a first key\n", first ? "PASS" : "FAIL");
  if (!first) failures++;

  /* replacing a latched chord is not a 'first key' (arp keeps its rhythm) */
  arp_init(&a); arp_set_latch(&a, 1); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  first = arp_note_on(&a, 0, 50, 100);
  printf("%s: replacing a latched chord keeps the rhythm (not a first key)\n", !first ? "PASS" : "FAIL");
  if (first) failures++;

  /* latch off -> behaves exactly as before */
  arp_init(&a); arp_set_latch(&a, 1); arp_set_latch(&a, 0); chord(&a);
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  n = play(&a, 0, 960, g);
  { static const int e[] = { 0 }; expect("latch off: releasing keys stops the arp", g, n, e, 0); }
}

/* ---------- stage 4a: pattern steps ---------- */

static void test_pattern_shift_example(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_length(&a, 4);
  arp_set_step(&a, 2, 100, 12, 100, 0);
  arp_set_step(&a, 3, 100, -5, 100, 0);
  arp_process(&a, 0, 240 * 8, collect, &l);
  static const int exp[] = { 60, 64, 79, 55, 64, 67, 72, 59 };   /* C E G' G- E G C' B- */
  check_seq("pattern Length 4, shifts 0 0 +12 -5 over C-E-G (example table)", &l, exp, 8);
}

static void test_prob(void) {
  int g[512], n;
  ArpCore a;
  arp_init(&a); chord(&a);
  arp_set_length(&a, 2);
  arp_set_step(&a, 1, 0, 0, 100, 0);           /* every 2nd step silent */
  n = play(&a, 0, 240 * 6, g);
  { static const int e[] = { 60, 67, 64 }; expect("prob 0%: step silent, note order still advances", g, n, e, 3); }

  Log l1 = {0}, l2 = {0};
  for (int k = 0; k < 2; k++) {
    arp_init(&a); chord(&a); arp_seed(&a, 1234);
    arp_set_length(&a, 1);
    arp_set_step(&a, 0, 50, 0, 100, 0);
    arp_process(&a, 0, 240 * 400, collect, k ? &l2 : &l1);
  }
  n = ons(&l1, g);
  printf("%s: prob 50%%: about half of 400 steps play (%d)\n", n > 160 && n < 240 ? "PASS" : "FAIL", n);
  printf("%s: prob 50%% is reproducible with the same seed\n", same_logs(&l1, &l2) ? "PASS" : "FAIL");
  failures += !(n > 160 && n < 240) + !same_logs(&l1, &l2);
}

static void test_shift_range(void) {
  int g[64], n;
  ArpCore a; arp_init(&a);
  arp_note_on(&a, 0, 120, 100); arp_note_on(&a, 0, 5, 100);
  arp_set_length(&a, 2);
  arp_set_step(&a, 0, 100, -24, 100, 0);       /* 5-24 < 0: silent */
  arp_set_step(&a, 1, 100, 24, 100, 0);        /* 120+24 > 127: silent */
  n = play(&a, 0, 240 * 4, g);
  { static const int e[] = { 0 }; expect("shift out of 0..127 makes the step silent", g, n, e, 0); }
  arp_set_step(&a, 0, 100, -60, 300, 50);       /* clamped to -24, 200 */
  printf("%s: step values are clamped\n", a.steps[0].shift == -24 && a.steps[0].vel == 200 ? "PASS" : "FAIL");
  if (!(a.steps[0].shift == -24 && a.steps[0].vel == 200)) failures++;
}

static void test_step_velocity(void) {
  ArpCore a; Log l = {0}; arp_init(&a);
  arp_note_on(&a, 0, 60, 100);
  arp_set_length(&a, 4);
  arp_set_step(&a, 0, 100, 0, 50, 0);
  arp_set_step(&a, 1, 100, 0, 200, 0);         /* 200 -> clamped 127 */
  arp_set_step(&a, 2, 100, 0, 0, 0);           /* 0 -> 1 (not a note-off) */
  arp_set_step(&a, 3, 100, 0, 110, 0);
  arp_process(&a, 0, 240 * 4, collect, &l);
  int v[4], k = 0;
  for (int i = 0; i < l.n; i++) if ((l.ev[i].st & 0xF0) == 0x90 && k < 4) v[k++] = l.ev[i].d2;
  int ok = k == 4 && v[0] == 50 && v[1] == 127 && v[2] == 1 && v[3] == 110;
  printf("%s: step velocity %% of key velocity, clamped to 1..127\n", ok ? "PASS" : "FAIL");
  if (!ok) failures++;
}

static void test_step_gate(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_length(&a, 3);
  arp_set_step(&a, 0, 100, 0, 100, 0);         /* global 50% -> 120 */
  arp_set_step(&a, 1, 100, 0, 100, 10);        /* 24 */
  arp_set_step(&a, 2, 100, 0, 100, 100);       /* 240, off before next on */
  arp_process(&a, 0, 240 * 4, collect, &l);
  static const double exp[][3] = {
    {0, 1, 60}, {120, 0, 60}, {240, 1, 64}, {264, 0, 64}, {480, 1, 67}, {720, 0, 67}, {720, 1, 60}, {840, 0, 60},
  };
  check("step gate: Global, 10%, 100%", &l, exp, 8);
}

static void test_tie(void) {
  ArpCore a; Log l = {0}; arp_init(&a); chord(&a);
  arp_set_length(&a, 4);
  arp_set_step(&a, 0, 100, 0, 100, ARP_GATE_TIE);
  arp_set_step(&a, 1, 100, -4, 100, ARP_GATE_TIE);   /* 64-4 = 60: same pitch as the tied note */
  arp_set_step(&a, 2, 100, 0, 100, ARP_GATE_TIE);
  arp_set_step(&a, 3, 0, 0, 100, 0);                 /* silent: the tie ends here */
  arp_process(&a, 0, 240 * 5, collect, &l);
  /* step 1: 60 tied; step 2: 60 again -> continues, no events; step 3: legato 67 (on before off);
   * step 4 silent: tie ends; step 5 = pattern step 1 again: 64 tied (no off inside the range) */
  static const double exp2[][3] = { {0, 1, 60}, {480, 1, 67}, {480, 0, 60}, {720, 0, 67}, {960, 1, 64} };
  check("tie: same pitch continues, legato on-before-off, silent step ends it", &l, exp2, 5);

  Log l2 = {0};
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  arp_process(&a, 1200, 1500, collect, &l2);
  static const double e3[][3] = { {1200, 0, 64} };
  check("tie: keys released -> tied note ends at the next step", &l2, e3, 1);

  ArpCore b; Log l3 = {0}; arp_init(&b); chord(&b);
  arp_set_step(&b, 0, 100, 0, 100, ARP_GATE_TIE);
  arp_process(&b, 0, 100, collect, &l3);
  arp_silence(&b, 100, collect, &l3);
  arp_process(&b, 100, 200, collect, &l3);
  static const double e4[][3] = { {0, 1, 60}, {100, 0, 60} };
  check("tie: transport jump silences the tied note", &l3, e4, 2);
}

static void test_pattern_reset_and_length(void) {
  int g[64], n;
  ArpCore a; arp_init(&a); chord(&a);
  arp_set_length(&a, 4);
  arp_set_step(&a, 0, 100, 12, 100, 0);
  n = play(&a, 0, 240 * 2, g);                 /* steps 1,2: 72 64 */
  arp_note_off(&a, 0, 60); arp_note_off(&a, 0, 64); arp_note_off(&a, 0, 67);
  chord(&a);
  n = play(&a, 480, 480 + 240 * 2, g);         /* restarts at step 1: 72 64 */
  { static const int e[] = { 72, 64 }; expect("pattern restarts at step 1 after all keys released", g, n, e, 2); }

  arp_set_length(&a, 1);                       /* only step 1 (+12) now */
  n = play(&a, 960, 960 + 240 * 3, g);         /* order continues 67 60 64, all +12 */
  { static const int e[] = { 79, 72, 76 }; expect("Length changed on the fly", g, n, e, 3); }
}

int main(void) {
  test_up_one_block();
  test_chunks();
  test_add_release();
  test_insert_below();
  test_silence();
  test_late_off();
  test_release_all();
  test_orders();
  test_single_note_orders();
  test_played();
  test_random();
  test_rates();
  test_gates();
  test_live_changes();
  test_high_notes();
  test_wide_chord_octaves();
  test_latch();
  test_pattern_shift_example();
  test_prob();
  test_shift_range();
  test_step_velocity();
  test_step_gate();
  test_tie();
  test_pattern_reset_and_length();
  printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
