/* COWBELL's sequencer, tested natively: cowbell.c is included whole (its statics are ours to read), driven
 * through ui() / midi() / render() with a fake host, 16 frames (one control chunk) a render() call.
 * Every hit (start_hit) is logged with its frame, part, velocity and pitch, read back from the voice it took.
 * Each test runs in its own process (fork), so each starts from init() with nothing left over.
 *
 *   wsl -d Ubuntu -- bash /mnt/c/code/claude/fm1/tests/run.sh            all tests
 *   wsl -d Ubuntu -- bash /mnt/c/code/claude/fm1/tests/run.sh ratchet    the ones whose name has "ratchet"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

#include "../cowbell/cowbell.c"

/* ---- a fake bank: two long, silent samples (the timing is what is tested, not the sound) */
static const uint8_t ZB[100000];
const cb_sample_t CB_SAMPLES[] = {
    {"ALPHA", 60, 128, 0, 0, 200000, ZB},
    {"BETA", 60, 128, 0, 0, 200000, ZB},
};
const uint32_t CB_N_SAMPLES = 2;

/* ---- the fake host */
static uint64_t T;                              /* frames since power-on */
static uint32_t h_ms(void) { return (uint32_t)(T * 1000u / SR); }
static uint32_t h_ticks(void) { return (uint32_t)(T * 24000000u / SR); }
static void h_log(const char *s) { (void)s; }
static void h_log_u32(const char *s, uint32_t v) { (void)s; (void)v; }
static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg) { (void)w; (void)h; (void)bg; }
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t fg) { (void)x; (void)y; (void)big; (void)s; (void)fg; }
static void h_cv_blit(int32_t x, int32_t y) { (void)x; (void)y; }
static void h_led(uint32_t id, uint32_t on) { (void)id; (void)on; }
static void h_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px) { (void)x; (void)y; (void)w; (void)h; (void)px; }
static int32_t h_text_w(uint32_t big, const char *s) { return (int32_t)strlen(s) * (big ? 16 : 8); }

#define MAXOUT 100000
static uint64_t mout_t[MAXOUT];
static uint8_t mout_st[MAXOUT];
static int n_mout;
static void h_midi_send(uint32_t st, uint32_t d1, uint32_t d2)
{
    (void)d1; (void)d2;
    if (n_mout < MAXOUT) { mout_t[n_mout] = T; mout_st[n_mout] = (uint8_t)st; n_mout++; }
}
static uint8_t bank[sizeof(store_t)];
static int bank_has, n_saves;
static int h_bank_load(void *p, uint32_t n) { if (!bank_has || n != sizeof bank) return -1; memcpy(p, bank, n); return 0; }
static int h_bank_save(const void *p, uint32_t n) { if (n != sizeof bank) return -1; memcpy(bank, p, n); bank_has = 1; n_saves++; return 0; }

static hb_host_t HOST;

/* ---- the hit log */
typedef struct { uint64_t t; int part, vel, semis, live; } hit_t2;
#define MAXH 2000000
static hit_t2 *HL;
static int NH;
static uint32_t ages_seen;

static int semis_of(uint32_t step)
{
    int i;
    for (i = 0; i < 73; i++) if (PITCH_Q16[i] == step) return i - 36;
    return 999;
}

/* the external clock (a master sending F8 to midi(), as the host does from its main loop) */
static double clk_period;                       /* frames a clock, 0: none */
static double clk_next;
static uint64_t clk_t[200000];                  /* the clocks sent */
static int n_clk;
static int fa_before_clock = -1;                /* send FA just before this clock (its index) */
static int fa_at = -1;                          /* the index of the first clock after FA */

static void chunk(void)
{
    int32_t out[2 * CH];
    uint32_t q, a, i, live;
    while (clk_period > 0 && clk_next <= (double)T) {
        if (n_clk == fa_before_clock) { midi(0xFAu, 0, 0); fa_at = n_clk; }
        midi(0xF8u, 0, 0);
        if (n_clk < 200000) clk_t[n_clk] = T;
        n_clk++;
        clk_next += clk_period;
    }
    if (T % 256u == 0) draw(0);                 /* the main loop, about once a block */
    q = hq_w - hq_r;                            /* render() takes the queued (live) hits first */
    memset(out, 0, sizeof out);
    render(out, CH);
    live = 0;
    for (a = ages_seen + 1u; a <= ages; a++) {
        hit_t2 *h = &HL[NH < MAXH ? NH : MAXH - 1];
        voice_t *v = 0;
        for (i = 0; i < NV; i++) if (V[i].age == a) v = &V[i];
        h->t = T;
        h->part = v ? v->part : -1;
        h->vel = v ? (int)(v->gain / 258) : -1;
        h->semis = v ? semis_of(v->step) : 999;
        h->live = live < q;
        live++;
        if (NH < MAXH) NH++;
    }
    ages_seen = ages;
    T += CH;
}
static void adv(uint64_t frames) { uint64_t end = T + frames; while (T < end) chunk(); }
static void adv_to(uint64_t t) { while (T < t) chunk(); }
static void adv_ms(uint32_t ms) { adv((uint64_t)ms * SR / 1000u); }

/* ---- the panel */
static void btn(uint32_t b) { ui(HB_EV_BTN_DOWN | b, 0); ui(HB_EV_BTN_UP | b, 0); }
static void func_btn(uint32_t b) { ui(HB_EV_BTN_DOWN | HB_BTN_GLO, 0); btn(b); ui(HB_EV_BTN_UP | HB_BTN_GLO, 0); }
static void kdown(uint32_t k) { ui(HB_EV_KEY_DOWN | k, 0); }
static void kup(uint32_t k) { ui(HB_EV_KEY_UP | k, 0); }
static void tap(uint32_t k) { kdown(k); kup(k); }
static void enc(uint32_t e, int32_t d) { ui(HB_EV_ENC | e, d); }
static const uint32_t W[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* step keys */
static const uint32_t B[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};                     /* part keys, ACC, MUTE, MORE! */

/* ---- checks */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("    FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void note_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void note_(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("    note: "); vprintf(fmt, ap); printf("\n"); va_end(ap); }

/* ---- what the pattern should play, worked out from the pattern alone (independent of the code's state) */
static const int RN[7] = {1, 2, 3, 4, 6, 8, 8};
static uint32_t slen_of(uint32_t bpm10, uint32_t swing, uint32_t s)
{
    uint32_t pair = 44100u * 300u / bpm10, first = pair * swing / 100u;
    return (s & 1u) ? pair - first : first;
}
static uint64_t up16(uint64_t x) { return (x + 15u) / 16u * 16u; }
static uint64_t ideal(const pat_t *pt, uint32_t nsteps)         /* frames from step 0 to the start of step nsteps */
{
    uint64_t a = 0; uint32_t s;
    for (s = 0; s < nsteps; s++) a += slen_of(pt->bpm * 10u, pt->swing, s % NS);
    return a;
}
static int audible_(const pat_t *pt, int p) { return pt->solo ? p + 1 == pt->solo : !pt->part[p].mute; }
static uint64_t gen_extra;                      /* gen_off()'s offset */
static int gen(const pat_t *pt, uint64_t T0, uint32_t from, uint32_t nsteps, hit_t2 *out, int max)
{
    int n = 0; uint32_t a, p, j;
    uint64_t start = gen_extra + ideal(pt, from);
    for (a = from; a < from + nsteps; a++) {
        uint32_t s = a % NS, l = slen_of(pt->bpm * 10u, pt->swing, s);
        for (p = 0; p < NP; p++) {
            const step_t *st = &pt->step[p][s];
            int code, base, nn;
            if (!st->on || !audible_(pt, (int)p)) continue;
            code = st->rat ? st->rat - 1 : pt->part[p].rat;
            if (code >= 7) code = 0;
            nn = RN[code];
            base = st->acc ? 127 : 96;
            for (j = 0; j < (uint32_t)nn && n < max; j++) {
                int vel = base, semis = (st->pitch == NO_LOCK ? 0 : st->pitch) + pt->part[p].pitch;
                if (code == 6) vel = base - base * 2 * (nn - 1 - (int)j) / (3 * (nn - 1));
                semis = semis < -36 ? -36 : semis > 36 ? 36 : semis;
                out[n].t = T0 + up16(start + (uint64_t)j * l / (uint32_t)nn);
                out[n].part = (int)p; out[n].vel = vel; out[n].semis = semis; out[n].live = 0;
                n++;
            }
        }
        start += l;
    }
    return n;
}
/* the same, the pattern's step `from` starting `off` frames after T0 (exactly, before the 16-frame rounding) */
static int gen_off(const pat_t *pt, uint64_t T0, uint64_t off, uint32_t from, uint32_t nsteps, hit_t2 *out, int max)
{
    int n;
    gen_extra = off;
    n = gen(pt, T0, from, nsteps, out, max);
    gen_extra = 0;
    return n;
}
static int cmp_hit(const void *a, const void *b)
{
    const hit_t2 *x = a, *y = b;
    if (x->t != y->t) return x->t < y->t ? -1 : 1;
    if (x->part != y->part) return x->part - y->part;
    return x->vel - y->vel;
}
/* the sequencer's hits in [t0, t1) against the expected ones; returns the mismatches */
static hit_t2 *EXP, *GOT;
static int compare(const char *what, const hit_t2 *exp, int ne, uint64_t t0, uint64_t t1)
{
    int ng = 0, i, bad = 0, shown = 0;
    for (i = 0; i < NH; i++) if (!HL[i].live && HL[i].t >= t0 && HL[i].t < t1) GOT[ng++] = HL[i];
    memcpy(EXP, exp, sizeof *exp * (size_t)ne);
    {   int k = 0; for (i = 0; i < ne; i++) if (EXP[i].t >= t0 && EXP[i].t < t1) EXP[k++] = EXP[i]; ne = k; }
    qsort(EXP, (size_t)ne, sizeof *EXP, cmp_hit);
    qsort(GOT, (size_t)ng, sizeof *GOT, cmp_hit);
    if (ne != ng) { bad++; printf("    FAIL %s: %d hits, expected %d\n", what, ng, ne); }
    for (i = 0; i < ne && i < ng; i++)
        if (EXP[i].t != GOT[i].t || EXP[i].part != GOT[i].part || EXP[i].vel != GOT[i].vel || EXP[i].semis != GOT[i].semis) {
            bad++;
            if (shown++ < 6)
                printf("    FAIL %s: hit %d: got t=%llu part=%d vel=%d semis=%d, expected t=%llu part=%d vel=%d semis=%d\n",
                       what, i, (unsigned long long)(GOT[i].t - t0), GOT[i].part, GOT[i].vel, GOT[i].semis,
                       (unsigned long long)(EXP[i].t - t0), EXP[i].part, EXP[i].vel, EXP[i].semis);
        }
    checks++;
    if (bad) fails++;
    return bad;
}
static hit_t2 *XP;
/* play from step 0 for nsteps, check every hit against the pattern */
static int play_and_check(const char *what, uint32_t nsteps)
{
    uint64_t T0;
    int ne;
    btn(HB_BTN_PLAY);
    T0 = T;
    adv_to(T0 + ideal(P, nsteps));
    ne = gen(P, T0, 0, nsteps, XP, MAXH);
    return compare(what, XP, ne, T0, T);
}
static int count_hits(uint64_t t0, uint64_t t1, int part, int live)
{
    int i, n = 0;
    for (i = 0; i < NH; i++)
        if (HL[i].t >= t0 && HL[i].t < t1 && (part < 0 || HL[i].part == part) && (live < 0 || HL[i].live == live)) n++;
    return n;
}
static void clear_all(pat_t *pt)
{
    uint32_t p, s;
    for (p = 0; p < NP; p++) for (s = 0; s < NS; s++) {
        pt->step[p][s].on = pt->step[p][s].acc = pt->step[p][s].rat = 0;
        pt->step[p][s].pitch = NO_LOCK;
    }
}

static void setup(void)
{
    uint32_t m, p;
    HOST.abi = HB_ABI; HOST.size = sizeof HOST;
    HOST.ms = h_ms; HOST.ticks = h_ticks; HOST.log = h_log; HOST.log_u32 = h_log_u32;
    HOST.fill = h_fill; HOST.cv_begin = h_cv_begin; HOST.cv_text = h_cv_text; HOST.cv_blit = h_cv_blit;
    HOST.led = h_led; HOST.midi_send = h_midi_send; HOST.blit = h_blit; HOST.text_w = h_text_w;
    HOST.bank_load = h_bank_load; HOST.bank_save = h_bank_save;
    HL = calloc(MAXH, sizeof *HL); EXP = calloc(MAXH, sizeof *EXP); GOT = calloc(MAXH, sizeof *GOT); XP = calloc(MAXH, sizeof *XP);
    T = 4608;                                   /* ~100 ms after power-on, on a main-loop tick */
    init(&HOST);
    for (m = 0; m < NMEM; m++) for (p = 0; p < NP; p++) S.mem[m].part[p].lvl = 127;   /* the log reads vel = gain / 258 */
    draw(1);
}

/* ================================================================ the tests */

static void t_demo(void)
{
    int i, n = 0;
    uint64_t T0;
    /* the demo groove (memory 1), three bars, every hit where and how loud the pattern says */
    play_and_check("demo, 3 bars", 48);
    T0 = HL[0].t;
    printf("    demo bar 1, parts 2 and 3 (step: hit offsets in the step, frames):\n");
    for (i = 0; i < NH && HL[i].t < T0 + ideal(P, 16); i++) {
        uint32_t s; uint64_t at = HL[i].t - T0;
        if (HL[i].part == 0) continue;
        for (s = 0; s < 16 && ideal(P, s + 1) <= at; s++) {}
        printf("      part %d step %2u +%5llu vel %d\n", HL[i].part + 1, s + 1, (unsigned long long)(at - ideal(P, s)), HL[i].vel);
        n++;
    }
    CHECK(n == 2 + 3 + 1 + 8, "parts 2-3 should hit 14 times in a bar (2 + x3, 1 + x8), got %d", n);
}

static void t_tempo_swing(void)
{
    static const uint16_t bpms[] = {40, 87, 120, 173, 300};
    static const uint8_t swings[] = {50, 58, 66, 75};
    uint32_t a, b, s;
    char what[64];
    for (a = 0; a < 5; a++)
        for (b = 0; b < 4; b++) {
            pid_t pid;
            fflush(stdout);
            pid = fork();
            if (pid == 0) {
                clear_all(P);
                for (s = 0; s < NS; s++) P->step[0][s].on = 1;
                P->step[3][5].on = 1; P->step[3][5].rat = 4;   /* x4 */
                P->bpm = bpms[a]; P->swing = swings[b];
                snprintf(what, sizeof what, "%u bpm swing %u, 2 bars", bpms[a], swings[b]);
                fflush(stdout), _exit(play_and_check(what, 32) ? 1 : 0);
            }
            { int st; waitpid(pid, &st, 0); checks++; if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++; }
        }
}

static void t_ratchets(void)
{
    int code, acc;
    char what[64];
    for (code = 0; code < 7; code++)
        for (acc = 0; acc < 2; acc++) {
            pid_t pid;
            fflush(stdout);
            pid = fork();
            if (pid == 0) {
                int i, nh = 0, last = -1, rising = 1;
                clear_all(P);
                P->step[2][4].on = 1; P->step[2][4].rat = (uint8_t)(code + 1); P->step[2][4].acc = (uint8_t)acc;
                P->step[2][9].on = 1;                        /* rat 0: the part's own (set below) */
                P->part[2].rat = (uint8_t)code;
                snprintf(what, sizeof what, "ratchet %s acc %d", RAT_S[code], acc);
                if (play_and_check(what, 16)) fflush(stdout), _exit(1);
                for (i = 0; i < NH; i++) if (HL[i].part == 2) {
                    if (nh < RN[code]) { if (HL[i].vel < last) rising = 0; last = HL[i].vel; }
                    nh++;
                }
                if (nh != 2 * RN[code]) { printf("    FAIL %s: %d hits\n", what, nh); fflush(stdout), _exit(1); }
                /* the ramp rises to the step's level */
                if (code == 6 && (!rising || last != (acc ? 127 : 96))) { printf("    FAIL %s: the ramp (last %d)\n", what, last); fflush(stdout), _exit(1); }
                fflush(stdout), _exit(0);
            }
            { int st; waitpid(pid, &st, 0); checks++; if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++; }
        }
    /* rat codes out of range in a saved pattern fall back to x1, not past the table */
    {
        pid_t pid; fflush(stdout); pid = fork();
        if (pid == 0) {
            clear_all(P);
            P->step[1][0].on = 1; P->step[1][0].rat = 200;
            P->part[1].rat = 99; P->step[1][8].on = 1;
            fflush(stdout), _exit(play_and_check("bad ratchet codes", 16) ? 1 : 0);
        }
        { int st; waitpid(pid, &st, 0); checks++; if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++; }
    }
}

static void t_accent_pitch(void)
{
    clear_all(P);
    P->step[0][0].on = 1; P->step[0][0].acc = 1;
    P->step[0][1].on = 1; P->step[0][1].pitch = 7;
    P->step[0][2].on = 1; P->step[0][2].pitch = -36;   /* + part pitch -5: clamped at -36 */
    P->step[0][3].on = 1; P->step[0][3].pitch = 36;
    P->step[1][4].on = 1; P->step[1][4].pitch = 0;     /* a lock at 0 is not "no lock" */
    P->part[0].pitch = -5;
    P->part[1].pitch = 12;
    play_and_check("accents and pitch locks", 32);
}

static void t_mute_solo(void)
{
    uint64_t t0;
    clear_all(P);
    P->step[0][0].on = P->step[1][4].on = P->step[2][8].on = 1;
    sel = 1;
    btn(HB_BTN_ENV);                                    /* mute part 2 */
    CHECK(P->part[1].mute == 1, "ENV mutes the selected part");
    play_and_check("part 2 muted", 16);
    btn(HB_BTN_PLAY);                                   /* stop */
    adv(1000);
    sel = 2;
    func_btn(HB_BTN_ENV);                               /* FUNC + ENV: solo part 3 */
    CHECK(P->solo == 3, "FUNC+ENV solos (solo = %d)", P->solo);
    play_and_check("part 3 solo", 16);
    btn(HB_BTN_PLAY);
    adv(1000);
    func_btn(HB_BTN_ENV);                               /* again: no solo */
    CHECK(P->solo == 0, "FUNC+ENV again ends the solo");
    btn(HB_BTN_SEQ);
    sel = 1;
    tap(B[9]);                                          /* the MUTE key in step mode: unmute part 2 */
    CHECK(P->part[1].mute == 0, "the MUTE key unmutes");
    t0 = T;
    play_and_check("all audible again", 16);
    CHECK(count_hits(t0, T, -1, 0) == 3, "3 hits");
}

static void t_step_entry(void)
{
    btn(HB_BTN_SEQ);
    CHECK(step_mode == 1, "SEQ: step mode");
    clear_all(P);
    tap(B[3]);                                          /* part 4 */
    CHECK(sel == 3, "the 4th black key selects part 4 (sel %d)", sel);
    tap(W[2]);
    CHECK(P->step[3][2].on == 1, "a tap sets step 3");
    tap(W[2]);
    CHECK(P->step[3][2].on == 0, "a second tap clears it");
    tap(W[5]);
    kdown(W[5]); enc(HB_ENC_SELECT, 5); kup(W[5]);     /* held: FEVER +5, the step stays on */
    CHECK(P->step[3][5].on == 1 && P->step[3][5].pitch == 5, "held step + FEVER locks the pitch (on %d pitch %d)",
          P->step[3][5].on, P->step[3][5].pitch);
    kdown(W[7]); enc(HB_ENC_ALGORITHM, 1); enc(HB_ENC_ALGORITHM, 1); kup(W[7]);   /* MORE: 2 clicks = 1 position */
    CHECK(P->step[3][7].on == 1 && P->step[3][7].rat == 2, "held step + MORE sets its ratchet to x2 (on %d rat %d)",
          P->step[3][7].on, P->step[3][7].rat);
    kdown(W[7]); enc(HB_ENC_ALGORITHM, -4); kup(W[7]);
    CHECK(P->step[3][7].rat == 1, "MORE turned down past x1 stops at x1 (rat %d)", P->step[3][7].rat);
    kdown(B[8]); tap(W[9]); tap(W[5]); kup(B[8]);       /* ACC held */
    CHECK(P->step[3][9].on == 1 && P->step[3][9].acc == 1, "ACC + an empty step: on, accented");
    CHECK(P->step[3][5].on == 1 && P->step[3][5].acc == 1, "ACC + a step: accented");
    kdown(B[8]); tap(W[5]); kup(B[8]);
    CHECK(P->step[3][5].acc == 0 && P->step[3][5].on == 1, "ACC again: the accent off, the step stays");
    btn(HB_BTN_OCT_UP);
    CHECK(sel == 4, "OCT+ in step mode: the next part");
    btn(HB_BTN_OCT_DOWN); btn(HB_BTN_OCT_DOWN);
    CHECK(sel == 2, "OCT- twice: back two parts");
    sel = 3;
    play_and_check("entered steps", 32);
    /* edits while playing are heard on the next pass of the step */
    {
        uint64_t T0 = T;
        tap(W[12]);
        adv_to(T0 + ideal(P, 16));
        CHECK(count_hits(T0, T, 3, 0) > 0, "plays on");
        btn(HB_BTN_PLAY);
    }
}

static void t_clear(void)
{
    int p, s, on = 0;
    sel = 1;
    btn(HB_BTN_SEL);
    for (s = 0; s < 16; s++) on += P->step[1][s].on;
    CHECK(on == 0, "SEL clears the part's steps");
    CHECK(P->step[0][0].on && P->step[2][6].on, "the other parts kept");
    func_btn(HB_BTN_SEL);
    for (p = 0, on = 0; p < 8; p++) for (s = 0; s < 16; s++) on += P->step[p][s].on + P->step[p][s].rat + (P->step[p][s].pitch != NO_LOCK);
    CHECK(on == 0, "FUNC+SEL clears every part");
}

/* REC in play mode: a key goes into the nearest step and is not heard twice */
static void t_rec_live(void)
{
    uint64_t T0, s5, s6, t;
    clear_all(P);
    P->step[0][0].on = 1;                               /* a reference hit at step 1 */
    sel = 2;
    btn(HB_BTN_REC);
    btn(HB_BTN_PLAY);
    T0 = T;
    s5 = T0 + ideal(P, 5); s6 = T0 + ideal(P, 6);
    adv_to(s5 + (s6 - s5) / 4);                         /* a quarter into step 6 (index 5): early */
    kdown(7); kup(7);                                   /* C4 */
    CHECK(P->step[2][5].on == 1 && P->step[2][5].pitch == 0, "an early hit goes into the step playing (on %d pitch %d)",
          P->step[2][5].on, P->step[2][5].pitch);
    t = T0 + ideal(P, 9);
    adv_to(t + (T0 + ideal(P, 10) - t) * 3 / 4);        /* three quarters into step 10: late */
    kdown(9); kup(9);                                   /* D4: +2 */
    CHECK(P->step[2][10].on == 1 && P->step[2][10].pitch == 2, "a late hit goes into the next step (on %d pitch %d)",
          P->step[2][10].on, P->step[2][10].pitch);
    CHECK(P->step[2][9].on == 0, "not into the step playing");
    adv_to(T0 + ideal(P, 16));
    CHECK(count_hits(T0, T, 2, 0) == 0, "this bar: part 3's live hits only, no repeat at step 11 (got %d sequenced)",
          count_hits(T0, T, 2, 0));
    CHECK(count_hits(T0, T, 2, 1) == 2, "2 live hits (got %d)", count_hits(T0, T, 2, 1));
    {
        int ne = gen(P, T0, 16, 16, XP, MAXH);
        adv_to(T0 + ideal(P, 32));
        compare("the recorded bar played back", XP, ne, T0 + ideal(P, 16), T);
    }
    /* ratchet motion: MORE turned while recording goes into the step playing, if that step is on */
    {
        uint64_t b = T0 + ideal(P, 32);
        adv_to(b + ideal(P, 3) + 100);                  /* step 4: off */
        enc(HB_ENC_ALGORITHM, 2);                       /* the part: x2 */
        CHECK(P->part[2].rat == 1, "MORE sets the part's ratchet (rat %d)", P->part[2].rat);
        CHECK(P->step[2][3].rat == 0 && P->step[2][3].on == 0, "motion leaves a step that is off alone (on %d rat %d)",
              P->step[2][3].on, P->step[2][3].rat);
        adv_to(b + ideal(P, 5) + 100);                  /* step 6: on (recorded above) */
        enc(HB_ENC_ALGORITHM, 2);                       /* the part: x3 */
        CHECK(P->step[2][5].rat == 3 && P->step[2][5].on == 1, "motion sets the playing step's ratchet: x3 (on %d rat %d)",
              P->step[2][5].on, P->step[2][5].rat);
    }
    btn(HB_BTN_PLAY);
}

/* a late REC hit into step 1 of the next bar, then STOP before it plays: is that step skipped on the next PLAY? */
static void t_rec_skip_leak(void)
{
    uint64_t T0, t;
    int got;
    clear_all(P);
    sel = 0;
    btn(HB_BTN_REC);
    btn(HB_BTN_PLAY);
    T0 = T;
    t = T0 + ideal(P, 15);
    adv_to(t + (T0 + ideal(P, 16) - t) * 3 / 4);        /* late in step 16 */
    tap(7);
    CHECK(P->step[0][0].on == 1, "recorded into step 1");
    btn(HB_BTN_PLAY);                                   /* stop before step 1 comes round */
    btn(HB_BTN_REC);
    adv(5000);
    btn(HB_BTN_PLAY);
    T0 = T;
    adv_to(T0 + ideal(P, 16));
    got = count_hits(T0, T, 0, 0);
    CHECK(got == 1, "after STOP and PLAY, the recorded step 1 plays (got %d hits; skip_next left set)", got);
    btn(HB_BTN_PLAY);
    adv(1000);                                          /* (STOP and PLAY in one chunk: only the STOP is kept) */
    /* the same with the part muted at the time: skip_next is only cleared on an audible step */
    clear_all(P);
    btn(HB_BTN_REC);
    btn(HB_BTN_PLAY);
    T0 = T;
    t = T0 + ideal(P, 3);
    adv_to(t + (T0 + ideal(P, 4) - t) * 3 / 4);         /* late in step 4 -> step 5 */
    tap(7);
    P->part[0].mute = 1;                                /* muted before step 5 plays */
    adv_to(T0 + ideal(P, 16));
    P->part[0].mute = 0;
    btn(HB_BTN_REC);
    {
        uint64_t b = T;
        adv_to(T0 + ideal(P, 32));
        got = count_hits(b, T, 0, 0);
        CHECK(got == 1, "recorded, muted, unmuted: the step plays next bar (got %d)", got);
    }
    btn(HB_BTN_PLAY);
}

static void t_stop_continue(void)
{
    uint64_t T0, ts, t1, tc;
    int ne;
    clear_all(P);
    {   uint32_t s; for (s = 0; s < 16; s++) P->step[0][s].on = 1; }
    P->step[1][6].on = 1; P->step[1][6].rat = 4;        /* x4 at step 7 */
    btn(HB_BTN_PLAY);
    T0 = T;
    ts = T0 + ideal(P, 6) + slen_of(1200, 50, 6) / 2;  /* stop in the middle of step 7: two of its x4 played */
    adv_to(ts);
    btn(HB_BTN_PLAY);
    CHECK(count_hits(T0, T, 1, 0) == 2, "2 of the x4 before STOP (got %d)", count_hits(T0, T, 1, 0));
    t1 = T;
    adv(20000);
    CHECK(count_hits(t1, T, -1, 0) == 0, "silent while stopped");
    func_btn(HB_BTN_PLAY);                              /* FUNC+PLAY: continue */
    tc = T;
    adv(slen_of(1200, 50, 6) * 3);
    {
        int rest = count_hits(tc, tc + slen_of(1200, 50, 6) / 2 + 32, 1, 0);
        CHECK(rest == 2, "continue: the step's other 2 ratchet hits (got %d)", rest);
    }
    btn(HB_BTN_PLAY);
    adv(1000);
    btn(HB_BTN_PLAY);                                   /* PLAY: from step 1 */
    T0 = T;
    adv_to(T0 + ideal(P, 16));
    ne = gen(P, T0, 0, 16, XP, MAXH);
    compare("PLAY after continue starts at step 1", XP, ne, T0, T);
    btn(HB_BTN_PLAY);
}

static void t_memories(void)
{
    uint64_t T0, t;
    int ne;
    /* FUNC + step key 2: memory 2 (empty), while playing: silence, then back to 1 at the same place */
    btn(HB_BTN_SEQ);
    btn(HB_BTN_PLAY);
    T0 = T;
    adv_to(T0 + ideal(P, 4));
    ui(HB_EV_BTN_DOWN | HB_BTN_GLO, 0); tap(W[1]); ui(HB_EV_BTN_UP | HB_BTN_GLO, 0);
    CHECK(cur_mem == 1 && P == &S.mem[1], "FUNC+step 2: memory 2");
    t = T;
    adv_to(T0 + ideal(P, 8));
    CHECK(count_hits(t, T, -1, 0) == 0, "memory 2 is empty: silent");
    ui(HB_EV_BTN_DOWN | HB_BTN_GLO, 0); tap(W[0]); ui(HB_EV_BTN_UP | HB_BTN_GLO, 0);
    CHECK(cur_mem == 0, "back to memory 1");
    t = T;
    adv_to(T0 + ideal(P, 16));
    ne = gen(P, T0, 8, 8, XP, MAXH);
    compare("memory 1 again, in time", XP, ne, t, T);
    ui(HB_EV_BTN_DOWN | HB_BTN_GLO, 0); tap(W[10]); ui(HB_EV_BTN_UP | HB_BTN_GLO, 0);
    CHECK(cur_mem == 0, "FUNC+step 11: no memory 11 (cur %d)", cur_mem);
    /* WRITE: refused while playing, done when stopped; it all comes back after a power cycle */
    btn(HB_BTN_SAVE);
    adv(512);
    CHECK(n_saves == 0, "no save while playing");
    btn(HB_BTN_PLAY);
    S.mem[3].step[5][9].on = 1; S.mem[3].bpm = 99;
    btn(HB_BTN_SAVE);
    adv(512);
    CHECK(n_saves == 1, "saved when stopped (%d)", n_saves);
    memset(&S, 0, sizeof S);
    init(&HOST);
    CHECK(S.mem[3].step[5][9].on == 1 && S.mem[3].bpm == 99 && S.mem[0].step[1][14].rat == 3, "loaded back");
    /* Not done yet (known gap): PLAY/STOP does not save the current memory's edits */
    n_saves = 0;
    P->step[7][7].on = 1;
    btn(HB_BTN_PLAY); adv(5000); btn(HB_BTN_PLAY); adv(5000);
    if (n_saves != 1) note_("known gap: PLAY/STOP does not save the edits (%d saves)", n_saves);
}

static void t_clock_out(void)
{
    uint64_t T0;
    int i, n = 0, fa = 0, fc = 0, worst = 0;
    btn(HB_BTN_PLAY);
    T0 = T;
    adv_to(T0 + ideal(P, 32));
    adv(256);                                           /* the last ones go out at the next draw */
    btn(HB_BTN_PLAY);
    adv(256);
    for (i = 0; i < n_mout; i++) {
        if (mout_st[i] == 0xF8u && mout_t[i] < T0 + ideal(P, 32) + 256) {
            int64_t want = (int64_t)(T0 + (uint64_t)n * 11025u / 12u), err = (int64_t)mout_t[i] - want;
            if (err < 0) err = -err;
            if (err > worst) worst = (int)err;
            n++;
        }
        if (mout_st[i] == 0xFAu) fa++;
        if (mout_st[i] == 0xFCu) fc++;
    }
    CHECK(n == 192 || n == 193, "2 bars: 192 clocks out (got %d)", n);
    CHECK(worst <= 300, "clocks out within a block of where they belong (worst %d frames)", worst);
    if (fa != 1 || fc != 1) note_("known gap: no MIDI Start / Stop (FA / FC) out on PLAY / STOP (FA %d, FC %d)", fa, fc);
}

/* follow a master's clock: does step k start on the master's clock 6k + 1 (the first clock after FA is the downbeat)? */
static void ext_follow(double bpm, uint32_t swing, const char *what)
{
    uint64_t start;
    int i, k, nst = 0;
    int64_t off[64];
    uint32_t s;
    clear_all(P);
    for (s = 0; s < 16; s++) P->step[0][s].on = 1;
    P->swing = (uint8_t)swing;
    clk_period = SR * 60.0 / (bpm * 24.0);
    clk_next = (double)T;
    fa_before_clock = 48;                               /* two beats of clock first, then FA and the downbeat clock */
    adv((uint64_t)(clk_period * (48 + 24 * 16)));
    CHECK(ext_bpm10 > 0, "%s: an external tempo (%u)", what, ext_bpm10);
    start = clk_t[fa_at];
    for (i = 0; i < NH; i++) if (!HL[i].live && HL[i].t + 32 >= start && nst < 64) {
        off[nst++] = (int64_t)HL[i].t;
    }
    printf("    %s: ext tempo %u.%u; step start - its clock (frames; 1 clock = %.0f):", what, ext_bpm10 / 10, ext_bpm10 % 10, clk_period);
    for (k = 0; k < nst; k++) {
        int c = fa_at + 6 * k;
        if (c >= n_clk) break;
        off[k] -= (int64_t)clk_t[c];
        if (k < 6 || k % 8 == 0 || k >= 60) printf(" s%d:%lld", k, (long long)off[k]);
    }
    printf("\n");
    {   /* odd steps come late by the swing: (swing - 50) % of a pair of steps */
        int64_t worst = 0, late = (int64_t)(clk_period * 12.0 * (swing - 50) / 100.0);
        CHECK(nst >= 64, "%s: 64 steps played (%d)", what, nst);
        for (k = 32; k < 64 && k < nst; k++) {
            int64_t a = off[k] - (k & 1 ? late : 0);
            if (a < 0) a = -a;
            if (a > worst) worst = a;
        }
        CHECK(worst <= 64, "%s: after 2 bars, steps within 64 frames (1.5 ms) of the master's clock, swing allowed for (worst %lld)",
              what, (long long)worst);
    }
}
static void t_ext_clock(void)
{
    pid_t pid;
    struct { double bpm; uint32_t swing; const char *what; } c[] = {{100, 50, "ext 100 bpm"}, {137.5, 50, "ext 137.5 bpm"},
                                                                    {100, 66, "ext 100 bpm swung"}};
    int i;
    for (i = 0; i < 3; i++) {
        fflush(stdout);
        pid = fork();
        if (pid == 0) { int f0 = fails; ext_follow(c[i].bpm, c[i].swing, c[i].what); fflush(stdout), _exit(fails > f0); }
        { int st; waitpid(pid, &st, 0); checks++; if (!WIFEXITED(st) || WEXITSTATUS(st)) fails++; }
    }
    /* the clock goes away: back on the internal tempo; FC stops, FB continues */
    {
        uint64_t t;
        clk_period = SR * 60.0 / (100 * 24.0); clk_next = (double)T; fa_before_clock = 30;
        adv((uint64_t)(clk_period * 200));
        CHECK(playing, "FA started it");
        midi(0xFCu, 0, 0);
        adv(64);
        CHECK(!playing, "FC stops");
        midi(0xFBu, 0, 0);
        adv(64);
        CHECK(playing, "FB continues");
        clk_period = 0;
        adv_ms(600);
        CHECK(ext_bpm10 == 0, "no clock for 500 ms: internal tempo again");
        t = T;
        adv_ms(2000);
        CHECK(count_hits(t, T, -1, 0) > 0, "still playing on its own");
    }
}

static void t_hold_and_roll(void)
{
    uint64_t t0;
    int n;
    clear_all(P);
    sel = 4;
    P->part[4].rat = 3;                                 /* x4 */
    kdown(7);                                           /* play mode: a held key ratchets */
    t0 = T;
    adv(44100);                                         /* 1 s at 120: 8 steps, 32 hits at x4 */
    kup(7);
    n = count_hits(t0, T + 16, 4, -1);
    CHECK(n >= 32 && n <= 34, "a key held 1 s at x4: ~33 hits (got %d)", n);
    adv(10000);
    CHECK(count_hits(T - 9000, T, 4, -1) == 0, "released: stops");
    P->part[4].rat = 0;
    kdown(7); t0 = T; adv(44100); kup(7);
    CHECK(count_hits(t0, T + 16, 4, -1) == 1, "x1: one hit only (got %d)", count_hits(t0, T + 16, 4, -1));
    /* MORE! (the last black key) rolls the part in step mode */
    btn(HB_BTN_SEQ);
    P->part[4].rat = 5;                                 /* x8 */
    kdown(B[10]); t0 = T; adv(44100); kup(B[10]);
    n = count_hits(t0, T + 16, 4, -1);
    CHECK(n >= 64 && n <= 66, "MORE! held 1 s at x8: ~65 hits (got %d)", n);
    adv(10000);
    CHECK(count_hits(T - 9000, T, 4, -1) == 0, "MORE! released: stops");
}

static void t_tempo_change(void)
{
    uint64_t T0, b;
    int ne;
    clear_all(P);
    {   uint32_t s; for (s = 0; s < 16; s++) P->step[0][s].on = 1; }
    btn(HB_BTN_PLAY);
    T0 = T;
    adv_to(T0 + ideal(P, 16));                          /* 120 bpm: 88200 frames a bar */
    enc(HB_ENC_KNOB1, 30);                              /* 150 bpm, at the bar line */
    CHECK(P->bpm == 150, "KNOB1 +30: 150 bpm");
    b = T;
    adv_to(T0 + 88200u + ideal(P, 16));                 /* the new bar runs on from the exact bar line */
    ne = gen_off(P, T0, 88200u, 0, 16, XP, MAXH);
    compare("a bar at the new tempo", XP, ne, b, T);
    enc(HB_ENC_KNOB1, 1000);
    CHECK(P->bpm == 300, "tempo tops out at 300");
    enc(HB_ENC_KNOB1, -1000);
    CHECK(P->bpm == 40, "and bottoms out at 40");
    enc(HB_ENC_KNOB2, 100);
    CHECK(P->swing == 75, "swing tops out at 75");
    btn(HB_BTN_PLAY);
}

static void t_long_run(void)
{
    uint32_t p, s;
    int ne;
    uint64_t T0;
    clear_all(P);
    for (p = 0; p < NP; p++) for (s = 0; s < NS; s++) { P->step[p][s].on = 1; P->step[p][s].rat = (uint8_t)(1 + (p + s) % 7); }
    P->bpm = 300; P->swing = 61;
    btn(HB_BTN_PLAY);
    T0 = T;
    adv_to(T0 + ideal(P, 16 * 375));                    /* 375 bars at 300 bpm: 5 minutes, every part every step, all ratchets */
    ne = gen(P, T0, 0, 16 * 375, XP, MAXH);
    compare("5 minutes at 300 bpm, 8 parts, every ratchet", XP, ne, T0, T);
    printf("    %d hits in 5 minutes, %d expected, the last bar line at frame %llu\n", NH, ne, (unsigned long long)(T - T0));
}

/* ================================================================ */

typedef struct { const char *name; void (*fn)(void); } test_t;
static const test_t TESTS[] = {
    {"demo", t_demo}, {"tempo_swing", t_tempo_swing}, {"ratchets", t_ratchets}, {"accent_pitch", t_accent_pitch},
    {"mute_solo", t_mute_solo}, {"step_entry", t_step_entry}, {"clear", t_clear}, {"rec_live", t_rec_live},
    {"rec_skip_leak", t_rec_skip_leak}, {"stop_continue", t_stop_continue}, {"memories", t_memories},
    {"clock_out", t_clock_out}, {"ext_clock", t_ext_clock}, {"hold_and_roll", t_hold_and_roll},
    {"tempo_change", t_tempo_change}, {"long_run", t_long_run},
};

int main(int argc, char **argv)
{
    unsigned i;
    int failed = 0, run = 0;
    for (i = 0; i < sizeof TESTS / sizeof TESTS[0]; i++) {
        pid_t pid;
        int st;
        if (argc > 1 && !strstr(TESTS[i].name, argv[1])) continue;
        printf("%s\n", TESTS[i].name);
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            setup();
            TESTS[i].fn();
            printf("  %s: %d checks, %d failed\n", TESTS[i].name, checks, fails);
            fflush(stdout);
            fflush(stdout), _exit(fails ? 1 : 0);
        }
        waitpid(pid, &st, 0);
        run++;
        if (!WIFEXITED(st) || WEXITSTATUS(st)) { failed++; if (!WIFEXITED(st)) printf("  %s: CRASHED (signal %d)\n", TESTS[i].name, WTERMSIG(st)); }
    }
    printf("\n%d tests, %d failed\n", run, failed);
    return failed != 0;
}
