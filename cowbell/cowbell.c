/* SPDX-License-Identifier: GPL-3.0-only */
/* COWBELL: a sample cowbell groovebox for fm1-dock, after the volca cowbell meme. See DESIGN.md.
 *
 *   8 parts, each a cowbell sample (PRESCRIPTION), pitch-shifted (FEVER), ratcheted (MORE), with a decay.
 *   A 16-step sequencer on the white keys; the black keys pick the parts.
 *
 * Threads: render() runs in the audio interrupt; ui(), draw() and midi() in the main loop. The main loop
 * talks to render() through byte-sized fields (atomic) and a one-way ring of hits (hitq). */
#include "hb_module.h"
#include "samples.h"
#include "art.h"

void *memset(void *d, int c, __SIZE_TYPE__ n);   /* modules/lib/hb_lib.c */

/* render() and everything it calls, and the tables it reads: the FM-1 firmware build (fw/) puts them in
 * internal RAM (its SDK's rule for audio code); in fm1-dock they stay where the module is */
#ifndef CB_AUDIO
#define CB_AUDIO
#define CB_AUDIO_DATA
#endif

static const hb_host_t *H;

#define NP    8u                               /* parts */
#define NS    16u                               /* steps */
#define NMEM  10u                               /* pattern memories */
#define NV    16u                               /* voices */
#define CH    16u                               /* frames per control chunk: envelopes and the sequencer */
#define SR    44100u
#define NO_LOCK (-128)

/* ---- tables, worked out on the computer */
static const uint32_t PITCH_Q16[73] CB_AUDIO_DATA = {         /* 2^(s/12) x 65536, s = -36..+36 */
    8192,8679,9195,9742,10321,10935,11585,12274,13004,13777,14596,15464,16384,17358,18390,19484,20643,21870,
    23170,24548,26008,27554,29193,30929,32768,34716,36781,38968,41285,43740,46341,49097,52016,55109,58386,61858,
    65536,69433,73562,77936,82570,87480,92682,98193,104032,110218,116772,123715,131072,138866,147123,155872,
    165140,174960,185364,196386,208064,220436,233544,247431,262144,277732,294247,311744,330281,349920,370728,
    392772,416128,440872,467088,494862,524288};
static const int32_t DECAY_Q15[16] CB_AUDIO_DATA = {          /* the level's factor per chunk: to -60 dB in 30 ms .. 1.6 s, 15: none */
    30142,30993,31427,31757,31957,32142,32288,32397,32476,32541,32590,32631,32666,32693,32717,32768};
static const int16_t SIN_Q14[91] = {            /* sin(0..90 degrees) x 16384 */
    0,286,572,857,1143,1428,1713,1997,2280,2563,2845,3126,3406,3686,3964,4240,4516,4790,5063,5334,5604,5872,6138,
    6402,6664,6924,7182,7438,7692,7943,8192,8438,8682,8923,9162,9397,9630,9860,10087,10311,10531,10749,10963,11174,
    11381,11585,11786,11982,12176,12365,12551,12733,12911,13085,13255,13421,13583,13741,13894,14044,14189,14330,
    14466,14598,14726,14849,14968,15082,15191,15296,15396,15491,15582,15668,15749,15826,15897,15964,16026,16083,
    16135,16182,16225,16262,16294,16322,16344,16362,16374,16382,16384};
static const int16_t IMA_STEP[89] CB_AUDIO_DATA = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,
    209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,
    2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767};
static const int8_t IMA_IDX[8] CB_AUDIO_DATA = {-1, -1, -1, -1, 2, 4, 6, 8};

#define NRAT 7u
static const uint8_t RAT_N[NRAT] CB_AUDIO_DATA = {1, 2, 3, 4, 6, 8, 8};   /* MORE: hits a step; the last ramps up ("MORE!!") */
static const char *const RAT_S[NRAT] = {"x1", "x2", "x3", "x4", "x6", "x8", "!!"};

/* the keys: k = 0 (F3) .. 26 (G5) */
static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static int8_t key_step[27], key_black[27];      /* k -> step (white) or black index, -1: not one */

/* ---- the pattern, as saved */
typedef struct { uint8_t smp, dec, rat, send, mute, lvl; int8_t pitch; uint8_t pad; } part_t;
typedef struct { uint8_t on, acc, rat; int8_t pitch; } step_t;   /* rat 0: the part's, else code + 1; pitch: NO_LOCK */
typedef struct {
    part_t part[NP];
    step_t step[NP][NS];
    uint16_t bpm;
    uint8_t swing, rev, solo, pad[3];           /* solo: part + 1, 0: none */
} pat_t;
#define STORE_MAGIC 0x4C424357u                 /* "WCBL" */
typedef struct { uint32_t magic, version; pat_t mem[NMEM]; } store_t;
static store_t S;
static pat_t *volatile P;                       /* the memory playing */
static uint8_t cur_mem;

/* ---- voices */
typedef struct {
    const uint8_t *d;
    uint32_t n, pos;                            /* length, samples decoded so far */
    int32_t s0, s1, pred, idx;                  /* the last two decoded samples, the ADPCM state */
    uint32_t frac, step;                        /* Q16 phase between s0 and s1, its step */
    int32_t gain, env, dec;                     /* Q15: velocity x sample gain, the envelope, its factor a chunk */
    uint8_t on, part, send, keyv;               /* keyv: a played voice (several a part) */
    uint32_t age;
} voice_t;
static voice_t V[NV];
static uint32_t ages;

/* hits from the main loop into render(): a one-way ring */
typedef struct { uint8_t part, vel, keyv; int8_t semis; } hit_t;
#define HQ 32u
static hit_t hitq[HQ];
static volatile uint32_t hq_w, hq_r;

/* ---- the sequencer (render's) */
static volatile uint8_t playing, cur_step, want_play, want_stop, want_cont;
static uint32_t sfr, slen;                      /* frames into the step, the step's length */
static uint8_t fresh;                           /* the first step is still to start */
static uint8_t r_n[NP], r_done[NP], r_ramp[NP], r_vel[NP];   /* this step's ratchet, per part */
static int8_t r_semis[NP];
static volatile uint8_t skip_next[NP];          /* a live hit recorded into the next step: it already sounded */
static volatile uint8_t roll_part = 0xFF;       /* MORE! held: this part rolls */
static volatile int8_t hold_k = -1;             /* play mode: the key held, repeated at the MORE rate */
static volatile int32_t hold_semis;
static volatile uint32_t hold_vel, hold_fr;
static uint32_t roll_fr;
static volatile uint32_t clk_out;               /* MIDI clocks due (24 a beat), sent by the main loop */
static uint32_t clk_sent;
static uint32_t clk_k;                          /* clocks given in this step (6 a step) */

/* external clock (main loop -> render) */
static volatile uint32_t ext_bpm10;             /* the incoming tempo x 10, 0: none */
static uint32_t ext_last_ms, ext_t24, ext_n;
static volatile uint32_t ext_mark;              /* clocks received since FA, read by render */
static uint8_t ext_start;                       /* FA came with a clock running: start on its next tick */
static uint32_t ext_mark_seen;

/* ---- the reverb: four combs and two allpasses (Freeverb's lengths), mono */
static int16_t cb0[1116], cb1[1188], cb2[1277], cb3[1356], ap0[556], ap1[441];
static int16_t *const CB[4] CB_AUDIO_DATA = {cb0, cb1, cb2, cb3};
static const uint16_t CBL[4] CB_AUDIO_DATA = {1116, 1188, 1277, 1356};
static uint16_t cbi[4], api0, api1;
static int32_t cbf[4];

/* ---- UI state (main loop) */
static uint8_t sel;                             /* the selected part */
static uint8_t func, acc_held, step_mode, rec;   /* step_mode 0: the keys play (the default); SEQ: steps */
static int8_t oct;
static int8_t held_step = -1;                   /* a white key held in step mode */
static uint8_t held_turned, held_was_on;
static int32_t more_clicks;                     /* MORE turns two clicks a position */
static uint32_t disp_until;                     /* the 7-segment shows a value until then (ms) */
static char disp[5];
static uint8_t save_req, saved_msg;
static uint32_t msg_until;

/* ================================================================ the sound */

static inline __attribute__((always_inline)) int32_t ima_next(voice_t *v)
{
    uint32_t b = v->d[v->pos >> 1];
    int32_t st, diff, p;
    b = (v->pos & 1u) ? b >> 4 : b & 15u;
    st = IMA_STEP[v->idx];
    diff = st >> 3;
    if (b & 4u) diff += st;
    if (b & 2u) diff += st >> 1;
    if (b & 1u) diff += st >> 2;
    p = v->pred + ((b & 8u) ? -diff : diff);
    v->pred = p > 32767 ? 32767 : p < -32768 ? -32768 : p;
    v->idx += IMA_IDX[b & 7u];
    v->idx = v->idx < 0 ? 0 : v->idx > 88 ? 88 : v->idx;
    v->pos++;
    return v->pred;
}

static CB_AUDIO void voice_cut(voice_t *v) { v->dec = 16384; }   /* -6 dB a chunk: gone in a few ms, no click */

static CB_AUDIO void start_hit(uint32_t p, int32_t semis, uint32_t vel, uint32_t keyv)
{
    const pat_t *pt = P;
    const part_t *pa = &pt->part[p];
    const cb_sample_t *sm;
    voice_t *v = 0;
    uint32_t i, nk = 0, best = 0;
    if (!CB_N_SAMPLES)
        return;
    sm = &CB_SAMPLES[pa->smp < CB_N_SAMPLES ? pa->smp : 0];
    for (i = 0; i < NV; i++) {                  /* a part cuts its own last hit; played keys keep four */
        voice_t *u = &V[i];
        if (!u->on || u->part != p || u->dec == 16384)
            continue;
        if (!keyv || !u->keyv) voice_cut(u);
        else nk++;
    }
    if (nk >= 4u) {
        voice_t *old = 0;
        for (i = 0; i < NV; i++)
            if (V[i].on && V[i].part == p && V[i].keyv && V[i].dec != 16384 && (!old || V[i].age < old->age))
                old = &V[i];
        if (old) voice_cut(old);
    }
    for (i = 0; i < NV && !v; i++)
        if (!V[i].on) v = &V[i];
    if (!v)                                     /* all busy: the quietest */
        for (i = 0; i < NV; i++)
            if (!v || (uint32_t)(32768 - V[i].env) > best) { best = (uint32_t)(32768 - V[i].env); v = &V[i]; }
    semis += pa->pitch;
    semis = semis < -36 ? -36 : semis > 36 ? 36 : semis;
    v->on = 0;
    v->d = sm->data;
    v->n = sm->n;
    v->pos = 0;
    v->pred = 0;
    v->idx = sm->index0;
    v->s0 = 0;
    v->s1 = v->n ? ima_next(v) : 0;
    v->frac = 0;
    v->step = PITCH_Q16[semis + 36] >> (sm->rshift & 1u);   /* a 22 kHz sample: half the steps */
    v->gain = (int32_t)(((vel * 258u) * sm->gain >> 7) * pa->lvl / 127u);
    if (v->gain > 32767) v->gain = 32767;       /* (s x gain stays in 32 bits) */
    v->env = 32768;
    v->dec = DECAY_Q15[pa->dec & 15u];
    v->part = (uint8_t)p;
    v->send = pa->send;
    v->keyv = (uint8_t)keyv;
    v->age = ++ages;
    v->on = v->n != 0;
}

static CB_AUDIO int audible(uint32_t p)
{
    const pat_t *pt = P;
    if (pt->solo) return p + 1u == pt->solo;
    return !pt->part[p].mute;
}

/* the step's length: a pair of steps is 2 x a 16th; swing gives the first more of it */
static CB_AUDIO uint32_t step_len(uint32_t s)
{
    const pat_t *pt = P;
    uint32_t bpm = ext_bpm10 ? ext_bpm10 : pt->bpm * 10u, pair, first;
    if (bpm < 200u) bpm = 200u;
    pair = SR * 300u / bpm;                     /* 2 x 60 / 4 / bpm s, bpm x 10 */
    first = pair * pt->swing / 100u;
    return (s & 1u) ? pair - first : first;
}

static CB_AUDIO void step_start(void)
{
    const pat_t *pt = P;
    uint32_t p, s = cur_step;
    slen = step_len(s);
    clk_k = 0;
    for (p = 0; p < NP; p++) {
        const step_t *st = &pt->step[p][s];
        uint32_t code;
        r_n[p] = r_done[p] = 0;
        if (skip_next[p]) { skip_next[p] = 0; continue; }   /* for this step only, whatever it holds */
        if (!st->on || !audible(p))
            continue;
        code = st->rat ? st->rat - 1u : pt->part[p].rat;
        if (code >= NRAT) code = 0;
        r_n[p] = RAT_N[code];
        r_ramp[p] = code == NRAT - 1u;
        r_vel[p] = st->acc ? 127u : 96u;
        r_semis[p] = st->pitch == NO_LOCK ? 0 : st->pitch;
    }
}

/* one control chunk of the sequencer: start steps, fire the ratchets' hits that are due */
static CB_AUDIO void seq_chunk(void)
{
    uint32_t p;
    if (want_play || want_stop)                 /* a live hit's skip is for the next step of this run only */
        for (p = 0; p < NP; p++) skip_next[p] = 0;
    if (want_play) { want_play = 0; cur_step = 0; sfr = 0; fresh = 1; playing = 1; ext_mark_seen = 0; }
    if (want_cont) { want_cont = 0; playing = 1; }
    if (want_stop) { want_stop = 0; playing = 0; }
    if (roll_part < NP) {                       /* MORE! held: the part at its MORE rate (a step without swing) */
        uint32_t code = P->part[roll_part].rat, every;
        if (code >= NRAT) code = 0;
        every = (step_len(0) + step_len(1)) / 2u / RAT_N[code];
        if (roll_fr >= every) {                 /* the remainder kept: no drift (the first hit: at once) */
            roll_fr = roll_fr >= 2u * every ? 0 : roll_fr - every;
            start_hit(roll_part, 0, 110u, 0);
        }
        roll_fr += CH;
    } else
        roll_fr = 0xFFFFFFu;
    if (hold_k >= 0) {                          /* play mode: a held key ratchets at MORE's rate (x1: once) */
        uint32_t code = P->part[sel].rat, every;
        if (code >= NRAT) code = 0;
        every = (step_len(0) + step_len(1)) / 2u / RAT_N[code];
        hold_fr += CH;
        if (RAT_N[code] > 1u && hold_fr >= every) { hold_fr -= every; start_hit(sel, hold_semis, hold_vel, 1); }
    }
    if (!playing)
        return;
    if (fresh) { fresh = 0; step_start(); }
    else if (sfr >= slen) {
        sfr -= slen;
        cur_step = (uint8_t)((cur_step + 1u) % NS);
        step_start();
    }
    /* following an external clock: 6 clocks a step, the first clock after FA the downbeat (clock 1), so step k
     * starts on clock 6k + 1. Checked at each pair of steps only (clock 12m + 1), where swing has no say;
     * err: how early we are (frames). Pull the phase a quarter of the way to it */
    if (ext_bpm10 && ext_mark && ext_mark != ext_mark_seen && ((ext_mark - 1u) % 12u) == 0) {
        uint32_t want = ((ext_mark - 1u) / 6u) % NS, have = cur_step;
        int32_t err;
        ext_mark_seen = ext_mark;
        if (have == want) err = (int32_t)sfr;                           /* the step already started */
        else if ((have + 1u) % NS == want) err = (int32_t)sfr - (int32_t)slen;   /* still in the one before */
        else err = 0x7FFFFFFF;
        if (err > (int32_t)slen || err < -(int32_t)slen) {      /* lost: jump */
            cur_step = (uint8_t)want; sfr = 0; step_start();
        } else {
            int32_t f = (int32_t)sfr - err / 4;
            sfr = f < 0 ? 0 : (uint32_t)f;
        }
    }
    while (clk_k < 6u && sfr >= clk_k * slen / 6u) { clk_k++; clk_out++; }
    for (p = 0; p < NP; p++)
        while (r_done[p] < r_n[p] && sfr >= r_done[p] * slen / r_n[p]) {
            uint32_t vel = r_vel[p];
            if (r_ramp[p])                      /* MORE!!: from a third of the level up to it (the last: all of it) */
                vel = vel - vel * 2u * (r_n[p] - 1u - r_done[p]) / (3u * (r_n[p] - 1u));
            start_hit(p, r_semis[p], vel, 0);
            r_done[p]++;
        }
    sfr += CH;
}

static inline __attribute__((always_inline)) int16_t sat16(int32_t x) { return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }

static CB_AUDIO void reverb(const int32_t *in, int32_t *out, uint32_t n, int32_t wet)
{
    uint32_t i, c;
    for (i = 0; i < n; i++) {
        int32_t x = in[i] >> 3, acc = 0, y, b;
        for (c = 0; c < 4u; c++) {              /* comb: feedback 0.84 through a damping low-pass */
            int16_t *buf = CB[c];
            y = buf[cbi[c]];
            cbf[c] = y + (((cbf[c] - y) * 13107) >> 15);
            buf[cbi[c]] = sat16(x + ((cbf[c] * 27525) >> 15));
            if (++cbi[c] >= CBL[c]) cbi[c] = 0;
            acc += y;
        }
        b = ap0[api0]; y = b - acc; ap0[api0] = sat16(acc + (b >> 1)); if (++api0 >= 556u) api0 = 0;
        acc = y;
        b = ap1[api1]; y = b - acc; ap1[api1] = sat16(acc + (b >> 1)); if (++api1 >= 441u) api1 = 0;
        out[i] += (y * wet) >> 7;
    }
}

static CB_AUDIO void render(int32_t *out, uint32_t n)
{
    int32_t mix[CH], snd[CH];
    uint32_t c, i, k;
    while (hq_r != hq_w) {                      /* the main loop's hits: auditions, keys, MIDI */
        const hit_t *h = &hitq[hq_r % HQ];
        start_hit(h->part, h->semis, h->vel, h->keyv);
        hq_r++;
    }
    for (c = 0; c + CH <= n; c += CH) {
        int32_t rev = P->rev;
        seq_chunk();
        for (i = 0; i < CH; i++) { mix[i] = 0; snd[i] = 0; __asm__ volatile("" ::: "memory"); }   /* no memset call */
        for (k = 0; k < NV; k++) {
            voice_t *v = &V[k];
            int32_t g0, g1, dg, env1;
            if (!v->on)
                continue;
            env1 = (v->env * v->dec) >> 15;     /* the envelope glides across the chunk: no steps */
            g0 = (v->gain * v->env) >> 15;
            g1 = (v->gain * env1) >> 15;
            dg = (g1 - g0) / (int32_t)CH;
            for (i = 0; i < CH; i++) {
                int32_t s = v->s0 + (((v->s1 - v->s0) * (int32_t)(v->frac >> 2)) >> 14);
                s = (s * g0) >> 15;             /* (pile-ups: the soft knee at the end) */
                mix[i] += s;
                if (v->send) snd[i] += s;
                g0 += dg;
                v->frac += v->step;
                while (v->frac >= 65536u) {
                    v->frac -= 65536u;
                    v->s0 = v->s1;
                    if (v->pos >= v->n) { v->on = 0; break; }
                    v->s1 = ima_next(v);
                }
                if (!v->on) break;
            }
            v->env = env1;
            if (env1 < 8) v->on = 0;            /* below -72 dB: done */
        }
        if (rev)
            reverb(snd, mix, CH, rev);
        for (i = 0; i < CH; i++) {              /* soft knee above -6 dB, then the limit */
            int32_t x = mix[i], a = x < 0 ? -x : x;
            if (a > 16384) a = 16384 + ((a - 16384) >> 2);
            if (a > 32767) a = 32767;
            x = x < 0 ? -a : a;
            out[2u * (c + i)] += x;
            out[2u * (c + i) + 1u] += x;
        }
    }
}

/* ================================================================ the main loop */

static void queue_hit(uint32_t p, int32_t semis, uint32_t vel, uint32_t keyv)
{
    hit_t *h;
    if (hq_w - hq_r >= HQ)
        return;
    h = &hitq[hq_w % HQ];
    h->part = (uint8_t)p; h->semis = (int8_t)semis; h->vel = (uint8_t)vel; h->keyv = (uint8_t)keyv;
    hq_w++;
}

static void show(const char *s)                 /* the 7-segment readout shows s for a moment */
{
    uint32_t i;
    for (i = 0; i < 4u; i++) disp[i] = *s ? *s++ : ' ';
    disp[4] = 0;
    disp_until = H->ms() + 1200u;
}

static void show_num(int32_t v, int sign)
{
    char b[5] = "    ";
    int32_t a = v < 0 ? -v : v, i = 3;
    do { b[i--] = (char)('0' + a % 10); a /= 10; } while (a && i >= 0);
    if (i >= 0 && (v < 0 || (sign && v > 0))) b[i] = v < 0 ? '-' : ' ';
    show(b);
}

/* the note C4 plays the sample at its own octave, the nearest C to its root */
static int32_t key_semis(uint32_t note)
{
    const part_t *pa = &P->part[sel];
    uint32_t root = pa->smp < CB_N_SAMPLES ? CB_SAMPLES[pa->smp].root : 60u;
    int32_t d = (int32_t)((root % 12u + 6u) % 12u) - 6;
    return (int32_t)note - 60 - d;
}

static void rec_hit(uint32_t p, int32_t semis_lock)   /* REC while playing: into the nearest step */
{
    pat_t *pt = P;
    uint32_t s = cur_step, f = sfr, l = slen ? slen : 1u;
    if (f > l / 2u) {
        s = (s + 1u) % NS;
        skip_next[p] = 1;
    }
    pt->step[p][s].on = 1;
    pt->step[p][s].acc = acc_held;
    if (semis_lock != NO_LOCK) pt->step[p][s].pitch = (int8_t)semis_lock;
}

static void key_down(uint32_t k, uint32_t vel)
{
    pat_t *pt = P;
    if (!vel) vel = 100u;
    if (!step_mode) {                           /* all 27 keys play the part chromatically */
        int32_t sm = key_semis(53u + k + (uint32_t)(oct * 12));
        queue_hit(sel, sm, vel, 1);
        if (rec && playing) rec_hit(sel, sm);
        hold_semis = sm; hold_vel = vel; hold_fr = 0;
        hold_k = (int8_t)k;
        return;
    }
    if (key_step[k] >= 0) {
        uint32_t s = (uint32_t)key_step[k];
        if (func) {                             /* FUNC + step 1..10: that memory */
            if (s < NMEM) { cur_mem = (uint8_t)s; P = &S.mem[s]; show_num((int32_t)s + 1, 0); }
            return;
        }
        step_t *st = &pt->step[sel][s];
        if (acc_held) { st->acc = !st->acc; if (!st->on) st->on = 1; return; }
        held_step = (int8_t)s;
        held_turned = 0;
        held_was_on = st->on;
        st->on = 1;                             /* on now; off on release unless a knob was turned */
        return;
    }
    if (key_black[k] >= 0) {
        uint32_t b = (uint32_t)key_black[k];
        if (b < NP) {
            sel = (uint8_t)b;
            queue_hit(b, 0, vel, 0);
            if (rec && playing) rec_hit(b, NO_LOCK);
        } else if (b == 8u) acc_held = 1;
        else if (b == 9u) {
            if (func) pt->solo = pt->solo == sel + 1u ? 0 : (uint8_t)(sel + 1u);
            else pt->part[sel].mute ^= 1u;
        } else if (b == 10u) roll_part = sel;
    }
}

static void key_up(uint32_t k)
{
    if (!step_mode) {
        if (hold_k == (int8_t)k) hold_k = -1;
        return;
    }
    if (key_step[k] >= 0 && held_step == key_step[k]) {
        step_t *st = &P->step[sel][(uint32_t)held_step];
        if (held_was_on && !held_turned) st->on = 0;
        held_step = -1;
    }
    if (key_black[k] == 8) acc_held = 0;
    if (key_black[k] == 10) roll_part = 0xFF;
}

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static void encoder(uint32_t id, int32_t d)
{
    pat_t *pt = P;
    part_t *pa = &pt->part[sel];
    step_t *hs = held_step >= 0 ? &pt->step[sel][(uint32_t)held_step] : 0;
    if (hs) held_turned = 1;
    switch (id) {
    case HB_ENC_SELECT: {                       /* FEVER: pitch (a held step: its own) */
        if (hs) {
            int32_t v = clampi((hs->pitch == NO_LOCK ? 0 : hs->pitch) + d, -36, 36);
            hs->pitch = (int8_t)v;
            show_num(v, 1);
        } else {
            pa->pitch = (int8_t)clampi(pa->pitch + d, -24, 24);
            show_num(pa->pitch, 1);
        }
        break;
    }
    case HB_ENC_PRESETS: {                      /* PRESCRIPTION: the sample, heard at once */
        int32_t v = (int32_t)pa->smp + d, n = (int32_t)CB_N_SAMPLES;
        if (n < 1) break;
        while (v < 0) v += n;
        pa->smp = (uint8_t)(v % n);
        show_num(pa->smp + 1, 0);
        queue_hit(sel, 0, 100u, 0);
        break;
    }
    case HB_ENC_ALGORITHM: {                    /* MORE: the ratchet, two clicks a position */
        int32_t code;
        more_clicks += d;
        if (more_clicks > -2 && more_clicks < 2) break;
        d = more_clicks / 2;
        more_clicks -= d * 2;
        if (hs) {
            code = clampi((hs->rat ? hs->rat - 1 : pa->rat) + d, 0, NRAT - 1);
            hs->rat = (uint8_t)(code + 1);
        } else {
            code = clampi(pa->rat + d, 0, NRAT - 1);
            pa->rat = (uint8_t)code;
            if (rec && playing && pt->step[sel][cur_step].on)   /* motion: the ratchet goes into the step playing */
                pt->step[sel][cur_step].rat = (uint8_t)(code + 1);
        }
        show(RAT_S[code]);
        break;
    }
    case HB_ENC_KNOB1:
        pt->bpm = (uint16_t)clampi(pt->bpm + d, 40, 300);
        show_num(pt->bpm, 0);
        break;
    case HB_ENC_KNOB2:
        pt->swing = (uint8_t)clampi(pt->swing + d, 50, 75);
        show_num(pt->swing, 0);
        break;
    case HB_ENC_KNOB3:
        pt->rev = (uint8_t)clampi(pt->rev + d * 4, 0, 128);
        show_num(pt->rev * 100 / 128, 0);
        break;
    case HB_ENC_KNOB4:
        pa->dec = (uint8_t)clampi(pa->dec + d, 0, 15);
        show_num(pa->dec, 0);
        break;
    }
}

static void button(uint32_t id, uint32_t down)
{
    pat_t *pt = P;
    if (id == HB_BTN_GLO) { func = (uint8_t)down; return; }
    if (!down)
        return;
    switch (id) {
    case HB_BTN_PLAY:
        if (playing) want_stop = 1;
        else if (func) want_cont = 1;
        else want_play = 1;
        break;
    case HB_BTN_REC: rec ^= 1u; break;
    case HB_BTN_SEQ: step_mode ^= 1u; held_step = -1; hold_k = -1; acc_held = 0; roll_part = 0xFF; break;
    case HB_BTN_FX: pt->part[sel].send ^= 1u; break;
    case HB_BTN_ENV:
        if (func) pt->solo = pt->solo == sel + 1u ? 0 : (uint8_t)(sel + 1u);
        else pt->part[sel].mute ^= 1u;
        break;
    case HB_BTN_SEL: {                          /* CLEAR: the part's steps (FUNC: every part's) */
        uint32_t p, s;
        for (p = 0; p < NP; p++)
            if (func || p == sel)
                for (s = 0; s < NS; s++) {
                    pt->step[p][s].on = pt->step[p][s].acc = pt->step[p][s].rat = 0;
                    pt->step[p][s].pitch = NO_LOCK;
                }
        break;
    }
    case HB_BTN_SAVE: save_req = 1; break;
    case HB_BTN_OCT_DOWN:
        if (!step_mode && !func) oct = (int8_t)clampi(oct - 1, -2, 2); else sel = (uint8_t)((sel + NP - 1u) % NP);
        break;
    case HB_BTN_OCT_UP:
        if (!step_mode && !func) oct = (int8_t)clampi(oct + 1, -2, 2); else sel = (uint8_t)((sel + 1u) % NP);
        break;
    }
}

static void ui(uint32_t ev, int32_t arg)
{
    uint32_t id = HB_EV_ID(ev);
    switch (HB_EV_KIND(ev)) {
    case HB_EV_KEY_DOWN: if (id < 27u) key_down(id, (uint32_t)arg); break;
    case HB_EV_KEY_UP:   if (id < 27u) key_up(id); break;
    case HB_EV_ENC:      encoder(id, arg); break;
    case HB_EV_BTN_DOWN: button(id, 1); break;
    case HB_EV_BTN_UP:   button(id, 0); break;
    }
}

/* MIDI: channel 10 notes 36..43 play the parts; other channels play the selected part chromatically.
 * Clock: F8 sets the tempo and the phase, FA / FB / FC start, continue, stop. */
static void midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint32_t kind = st & 0xF0u;
    if (st == 0xF8u) {
        uint32_t now = H->ticks();
        ext_last_ms = H->ms();
        ext_mark++;
        if (ext_start) { ext_start = 0; want_play = 1; }   /* FA, then this clock: the downbeat */
        if (++ext_n == 1u) ext_t24 = now;
        else if (ext_n == 25u) {                /* 24 clocks: one beat */
            uint32_t dt = now - ext_t24, b = dt ? 1440000000u / (dt / 10u + 1u) : 0;   /* bpm x 10 */
            if (b >= 300u && b <= 3000u)
                ext_bpm10 = ext_bpm10 ? (ext_bpm10 * 3u + b) / 4u : b;
            ext_t24 = now;
            ext_n = 1;
        }
        return;
    }
    if (st == 0xFAu) {                          /* with a clock running, start on its next tick; without, now */
        ext_mark = 0;
        if (ext_bpm10) ext_start = 1; else want_play = 1;
        return;
    }
    if (st == 0xFBu) { want_cont = 1; return; }
    if (st == 0xFCu) { ext_start = 0; want_stop = 1; return; }
    if (kind == 0x90u && d2) {
        if ((st & 15u) == 9u) {
            if (d1 >= 36u && d1 < 36u + NP) queue_hit(d1 - 36u, 0, d2, 0);
        } else
            queue_hit(sel, key_semis(d1), d2, 1);
    } else if (kind == 0xB0u && d1 == 123u) {
        uint32_t i;
        for (i = 0; i < NV; i++) voice_cut(&V[i]);
    }
}
static void note(uint32_t ch, uint32_t key, uint32_t vel) { midi((vel ? 0x90u : 0x80u) | (ch & 15u), key, vel); }
static void cc(uint32_t ch, uint32_t num, uint32_t val) { midi(0xB0u | (ch & 15u), num, val); }

/* ================================================================ the screen */

#define C_PANEL  CB_COL_PANEL
#define C_DARK   CB_COL_MORE
#define C_PLUM   HB_RGB(75, 21, 40)
#define C_LIGHT  HB_RGB(251, 234, 240)
#define C_LED    HB_RGB(44, 44, 42)
#define C_RED    HB_RGB(255, 60, 50)
#define C_REDOFF HB_RGB(80, 30, 28)
#define C_HEAD   HB_RGB(226, 75, 74)

/* text on a coloured ground, through the host's canvas (its text() paints black behind) */
static void txt(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint16_t bg, int center)
{
    int32_t h = big ? 32 : 16, tx = center ? (w - H->text_w(big, s)) / 2 : 0;
    H->cv_begin(w, h, bg);
    H->cv_text(tx < 0 ? 0 : tx, 0, big, s, fg);
    H->cv_blit(x, y);
}

/* the 7-segment readout: 4 digits at (8, 28), each 16 x 30 */
static const uint8_t SEG7[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
static void seg_digit(int32_t x, int32_t y, char ch)
{
    uint32_t m = ch >= '0' && ch <= '9' ? SEG7[ch - '0'] : ch == '-' ? 0x40u : ch == '!' ? 0x06u : 0;
    static const int8_t R[7][4] = {             /* segments a..g: x, y, w, h */
        {3, 0, 10, 3}, {13, 3, 3, 11}, {13, 16, 3, 11}, {3, 27, 10, 3}, {0, 16, 3, 11}, {0, 3, 3, 11}, {3, 13, 10, 3}};
    uint32_t i;
    if (ch == 'x') m = 0x76u;                   /* a ratchet's x: an H, near enough on seven segments */
    for (i = 0; i < 7u; i++)
        H->fill(x + R[i][0], y + R[i][1], R[i][2], R[i][3], (m >> i & 1u) ? C_RED : C_REDOFF);
}

/* the knobs: the picture expanded once, then the pointer drawn over a copy at each angle */
static uint16_t more_base[112 * 112], more_buf[112 * 112];
static uint16_t small_base[44 * 44], small_buf[44 * 44];

static void expand(const cb_img_t *img, uint16_t *dst)
{
    uint32_t x, y, rb = (img->w + 1u) / 2u;
    for (y = 0; y < img->h; y++)
        for (x = 0; x < img->w; x++) {
            uint32_t b = img->px[y * rb + x / 2u];
            dst[y * img->w + x] = img->pal[(x & 1u) ? b & 15u : b >> 4];
        }
}

static int32_t sin_deg(int32_t a)               /* x 16384 */
{
    while (a < 0) a += 360;
    while (a >= 360) a -= 360;
    if (a <= 90) return SIN_Q14[a];
    if (a <= 180) return SIN_Q14[180 - a];
    if (a <= 270) return -SIN_Q14[a - 180];
    return -SIN_Q14[360 - a];
}

static uint16_t blend(uint16_t a, uint16_t b, uint32_t t)   /* t 0..16: 16 is all b */
{
    uint32_t r = ((a >> 11) * (16u - t) + (b >> 11) * t) >> 4;
    uint32_t g = (((a >> 5) & 63u) * (16u - t) + ((b >> 5) & 63u) * t) >> 4;
    uint32_t bl = ((a & 31u) * (16u - t) + (b & 31u) * t) >> 4;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* a pointer from r0 to r1 at angle a (degrees, 0 up, clockwise), w16 its width x 16, anti-aliased */
static void knob_draw(const cb_img_t *img, const uint16_t *base, uint16_t *buf, int32_t sx, int32_t sy,
                      int32_t a, int32_t w16)
{
    int32_t s = sin_deg(a), c = sin_deg(a + 90);
    int32_t ax = img->cx * 16 + ((int32_t)img->r0 * s >> 10), ay = img->cy * 16 - ((int32_t)img->r0 * c >> 10);
    int32_t bx = img->cx * 16 + ((int32_t)img->r1 * s >> 10), by = img->cy * 16 - ((int32_t)img->r1 * c >> 10);
    int32_t dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, x, y, n = img->w * img->h;
    int32_t hi = w16 / 2 + 8, lo = w16 / 2 - 8, hi2 = hi * hi, lo2 = lo > 0 ? lo * lo : 0;
    int32_t x0 = ((ax < bx ? ax : bx) - hi) >> 4, x1 = ((ax > bx ? ax : bx) + hi) >> 4;
    int32_t y0 = ((ay < by ? ay : by) - hi) >> 4, y1 = ((ay > by ? ay : by) + hi) >> 4;
    for (x = 0; x < n; x++) buf[x] = base[x];
    if (len2 < 1) len2 = 1;
    for (y = y0 < 0 ? 0 : y0; y <= y1 && y < img->h; y++)
        for (x = x0 < 0 ? 0 : x0; x <= x1 && x < img->w; x++) {
            int32_t px = x * 16 - ax, py = y * 16 - ay, t = (px * dx + py * dy) * 256 / len2, qx, qy, d2;
            t = t < 0 ? 0 : t > 256 ? 256 : t;
            qx = px - (dx * t >> 8);
            qy = py - (dy * t >> 8);
            d2 = qx * qx + qy * qy;
            if (d2 >= hi2) continue;
            buf[y * img->w + x] = blend(buf[y * img->w + x], img->pointer,
                                        d2 <= lo2 ? 16u : (uint32_t)((hi2 - d2) * 16 / (hi2 - lo2)));
        }
    for (x = 0; x < n; x++) buf[x] = (uint16_t)(buf[x] >> 8 | buf[x] << 8);   /* the panel wants high byte first */
    H->blit(sx, sy, img->w, img->h, buf);
}

/* what is on the screen, to redraw only what changed */
static struct {
    int32_t more_a, fever_a, presc_a;
    char seg[5], head[32], left[8], right[16];
    uint8_t steps[NS], parts;
} drawn;
static int32_t more_a = -135;                   /* the MORE pointer, gliding to its value */
static uint8_t led_on[48];                      /* buttons 0..13, keys 14..40 */

static void led(uint32_t id, uint32_t on)
{
    if (id < 48u &&led_on[id] != on + 1u) { led_on[id] = (uint8_t)(on + 1u); H->led(id, on); }
}

static int strdiff(char *have, const char *want, uint32_t n)
{
    uint32_t i;
    int d = 0;
    for (i = 0; i < n; i++) {
        if (have[i] != want[i]) d = 1;
        have[i] = want[i];
        if (!want[i]) break;
    }
    return d;
}

static void draw(uint32_t full)
{
    const pat_t *pt = P;
    const part_t *pa = &pt->part[sel];
    uint32_t now = H->ms(), i, p;
    int32_t want, a;
    char b[32];

    if (full) {                                 /* everything again (also after the host's picker) */
        H->fill(0, 0, 240, 240, C_PANEL);
        H->fill(0, 0, 240, 20, C_DARK);
        txt(4, 2, 72, 0, "COWBELL", C_LIGHT, C_DARK, 0);
        H->fill(4, 24, 84, 38, C_LED);
        H->fill(60, 86, 120, 116, C_DARK);
        txt(60, 202, 120, 0, "MORE", C_LIGHT, C_PANEL, 1);
        txt(96, 70, 60, 0, "FEVER", C_PLUM, C_PANEL, 1);
        txt(160, 70, 80, 0, "PRESCRIPT", C_PLUM, C_PANEL, 1);
        memset(&drawn, 0xFF, sizeof drawn);
        drawn.more_a = drawn.fever_a = drawn.presc_a = 9999;
        memset(led_on, 0, sizeof led_on);
    }

    /* MIDI clock out, and the external clock going quiet */
    while (clk_sent != clk_out) {
        if (!ext_bpm10) H->midi_send(0xF8u, 0, 0);
        clk_sent++;
    }
    if (ext_bpm10 && now - ext_last_ms > 500u) { ext_bpm10 = 0; ext_n = 0; }

    /* WRITE: only while stopped (a flash save stalls the sound ~100 ms) */
    if (save_req && !playing) {
        save_req = 0;
        S.magic = STORE_MAGIC;
        S.version = 1;
        saved_msg = H->bank_save(&S, sizeof S) == 0;
        msg_until = now + 1500u;
    } else if (save_req && playing) {
        save_req = 0;
        saved_msg = 2;                          /* "STOP FIRST" */
        msg_until = now + 1500u;
    }

    /* the header: memory, transport, part and sample */
    {
        const char *nm = pa->smp < CB_N_SAMPLES ? CB_SAMPLES[pa->smp].name : "-";
        char *q = b;
        const char *t;
        if (now < msg_until)
            t = saved_msg == 1 ? "SAVED" : saved_msg == 2 ? "STOP FIRST" : "SAVE FAILED";
        else
            t = 0;
        *q++ = 'M'; if (cur_mem + 1u >= 10u) *q++ = '1'; *q++ = (char)('0' + (cur_mem + 1u) % 10u); *q++ = ' ';
        if (t) { while (*t) *q++ = *t++; }
        else {
            *q++ = playing ? '>' : '.'; *q++ = ' ';
            if (rec) { *q++ = 'R'; *q++ = 'E'; *q++ = 'C'; *q++ = ' '; }
            if (ext_bpm10) { *q++ = 'E'; *q++ = 'X'; *q++ = 'T'; *q++ = ' '; }
            if (step_mode) { *q++ = 'S'; *q++ = 'T'; *q++ = 'E'; *q++ = 'P'; *q++ = ' '; }
            t = nm;
            while (*t && q < b + 19) *q++ = *t++;      /* 160 px: 20 characters */
        }
        *q = 0;
        if (strdiff(drawn.head, b, sizeof drawn.head))
            txt(80, 2, 160, 0, b, C_LIGHT, C_DARK, 0);
    }

    /* the 7-segment: the tempo, or the value being turned */
    if (now < disp_until)
        for (i = 0; i < 5u; i++) b[i] = disp[i];
    else {
        uint32_t t = ext_bpm10 ? (ext_bpm10 + 5u) / 10u : pt->bpm;
        b[0] = ' ';
        b[1] = t >= 100u ? (char)('0' + t / 100u) : ' ';
        b[2] = (char)('0' + t / 10u % 10u);
        b[3] = (char)('0' + t % 10u);
        b[4] = 0;
    }
    if (strdiff(drawn.seg, b, sizeof drawn.seg))
        for (i = 0; i < 4u; i++) seg_digit(10 + (int32_t)i * 20, 28, b[i]);

    /* the knobs */
    {
        uint32_t code = pa->rat < NRAT ? pa->rat : 0;
        want = -135 + (int32_t)code * 45;
        a = more_a + (want - more_a + (want > more_a ? 2 : -2)) / 3;   /* glide */
        if (a == more_a) a = want;
        more_a = a;
        if (a != drawn.more_a) { drawn.more_a = a; knob_draw(&CB_ART_MORE, more_base, more_buf, 64, 88, a, 52); }
        a = -135 + (pa->pitch + 24) * 270 / 48;
        if (a != drawn.fever_a) { drawn.fever_a = a; knob_draw(&CB_ART_SMALL, small_base, small_buf, 104, 24, a, 40); }
        a = CB_N_SAMPLES > 1u ? -135 + (int32_t)(pa->smp * 270u / (CB_N_SAMPLES - 1u)) : 0;
        if (a != drawn.presc_a) { drawn.presc_a = a; knob_draw(&CB_ART_SMALL, small_base, small_buf, 178, 24, a, 40); }
    }

    /* left: the part; right: the ratchet, decay, reverb, mute */
    b[0] = 'P'; b[1] = (char)('1' + sel); b[2] = 0;
    if (strdiff(drawn.left, b, sizeof drawn.left))
        txt(4, 100, 52, 1, b, C_PLUM, C_PANEL, 1);
    {
        const char *r = RAT_S[pa->rat < NRAT ? pa->rat : 0];
        char *q = b;
        *q++ = r[0]; *q++ = r[1]; *q++ = ' ';
        *q++ = 'D'; *q++ = (char)('0' + pa->dec / 10u); *q++ = (char)('0' + pa->dec % 10u);
        *q++ = pa->send ? 'R' : ' ';
        *q++ = audible(sel) ? ' ' : 'M';
        *q = 0;
        if (strdiff(drawn.right, b, sizeof drawn.right)) {
            char l1[4] = {b[0], b[1], 0, 0}, l2[4] = {b[3], b[4], b[5], 0}, l3[3] = {b[6], b[7], 0};
            txt(184, 92, 52, 1, l1, C_PLUM, C_PANEL, 1);
            txt(184, 132, 52, 0, l2, C_PLUM, C_PANEL, 1);
            txt(184, 152, 52, 0, l3, C_PLUM, C_PANEL, 1);
        }
    }

    /* the 16 steps of the part (shown in both modes) */
    for (i = 0; i < NS; i++) {
        const step_t *st = &pt->step[sel][i];
        uint8_t s = (uint8_t)((st->on ? 1u : 0) | (st->acc ? 2u : 0) | (playing && cur_step == i ? 4u : 0) |
                              (st->rat || st->pitch != NO_LOCK ? 8u : 0));
        if (s != drawn.steps[i]) {
            uint16_t c = (s & 4u) ? C_HEAD : (s & 2u) && (s & 1u) ? HB_RGB(150, 20, 60) : (s & 1u) ? C_PLUM : C_DARK;
            drawn.steps[i] = s;
            H->fill((int32_t)i * 15, 220, 13, 18, c);
            if (s & 8u) H->fill((int32_t)i * 15 + 5, 214, 3, 3, C_PLUM);
            else H->fill((int32_t)i * 15 + 5, 214, 3, 3, C_PANEL);
        }
    }

    /* the LEDs: steps on the white keys (the playhead flips), parts on the black ones */
    for (i = 0; i < NS; i++) {
        uint32_t on = !step_mode ? 0 : P->step[sel][i].on ? 1u : 0;
        if (playing && cur_step == i) on = on ? 0 : 2u;
        led(HB_LED_KEY(WHITE[i]), on);
    }
    for (p = 0; p < NP; p++) {
        uint32_t any = 0;
        for (i = 0; i < NS && !any; i++) any = pt->step[p][i].on;
        led(HB_LED_KEY(BLACK[p]), !step_mode ? 0 : p == sel ? 1u : !audible(p) ? 0 : any ? 2u : 0);
    }
    led(HB_LED_KEY(BLACK[8]), acc_held);
    led(HB_LED_KEY(BLACK[9]), !audible(sel));
    led(HB_LED_KEY(BLACK[10]), roll_part < NP);
    led(HB_BTN_PLAY, playing);
    led(HB_BTN_REC, rec);
    led(HB_BTN_SEQ, step_mode);
    led(HB_BTN_FX, pa->send);
    led(HB_BTN_ENV, !audible(sel));
    led(HB_BTN_GLO, func);
    led(HB_BTN_SAVE, save_req);                 /* lit only while a save waits */
}

/* ================================================================ start and stop */

static void defaults(void)
{
    uint32_t m, p, s, n = CB_N_SAMPLES ? CB_N_SAMPLES : 1u;
    memset(&S, 0, sizeof S);
    for (m = 0; m < NMEM; m++) {
        pat_t *pt = &S.mem[m];
        pt->bpm = 120;
        pt->swing = 50;
        pt->rev = 24;
        for (p = 0; p < NP; p++) {
            pt->part[p].smp = (uint8_t)(p * n / NP);
            pt->part[p].dec = 15;
            pt->part[p].lvl = 110;
            for (s = 0; s < NS; s++) pt->step[p][s].pitch = NO_LOCK;
        }
    }
    /* memory 1: a groove to start from */
    for (s = 0; s < NS; s += 2u) S.mem[0].step[0][s].on = 1;
    S.mem[0].step[0][0].acc = S.mem[0].step[0][8].acc = 1;
    S.mem[0].step[1][3].on = S.mem[0].step[1][11].on = 1;
    S.mem[0].step[1][14].on = 1;
    S.mem[0].step[1][14].rat = 3;               /* x3 */
    S.mem[0].step[2][6].on = 1;
    S.mem[0].step[2][15].on = 1;
    S.mem[0].step[2][15].rat = 6;               /* x8 */
}

static int init(const hb_host_t *h)
{
    uint32_t k;
    H = h;
    for (k = 0; k < 27u; k++) key_step[k] = key_black[k] = -1;
    for (k = 0; k < 16u; k++) key_step[WHITE[k]] = (int8_t)k;
    for (k = 0; k < 11u; k++) key_black[BLACK[k]] = (int8_t)k;
    if (H->bank_load(&S, sizeof S) != 0 || S.magic != STORE_MAGIC || S.version != 1u)
        defaults();
    cur_mem = 0;
    P = &S.mem[0];
    expand(&CB_ART_MORE, more_base);
    expand(&CB_ART_SMALL, small_base);
    H->log_u32("cowbell: samples ", CB_N_SAMPLES);
    return 0;
}

static void release(void)
{
    uint32_t i;
    playing = 0;
    roll_part = 0xFF;
    for (i = 0; i < NV; i++) V[i].on = 0;
}

HB_DESC = {HB_ABI, "COWBELL", init, release, note, cc, render, ui, draw, 0, 0, HB_F_SCREEN | HB_F_PANEL, midi};
