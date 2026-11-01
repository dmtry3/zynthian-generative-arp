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

int main(void) {
  test_up_one_block();
  test_chunks();
  test_add_release();
  test_insert_below();
  test_silence();
  test_late_off();
  test_release_all();
  printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
  return failures ? 1 : 0;
}
