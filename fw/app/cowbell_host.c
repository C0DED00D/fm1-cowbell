/* SPDX-License-Identifier: GPL-3.0-only */
/* COWBELL as the FM-1's own firmware, on M-VAVE's fm1_open SDK.
 *
 * cowbell/cowbell.c is the same source as the fm1-dock module. This file is the host it expects (hb_module.h:
 * screen, LEDs, the pattern store, MIDI, the clock) built on the SDK, and the SDK's application hooks
 * (groove_*: the SDK's control task calls them) in place of M-VAVE's groovebox UI:
 *   groove_screen_control()  the control loop: the panel into ui() events, MIDI into midi(), draw()
 *   cowbell_block()          the audio interrupt (board/audio.c): render() into the 128-frame block
 *
 * The user partition of the flash (fm1_flash_*: 536,576 bytes):
 *   0x0000  the pattern store, slot A (2 sectors)      0x2000  slot B: saves alternate, the newer valid one loads
 *   0x5000  the sample bank (tools/make_samples.py writes it, tools/cowbell_upload.py or the web uploader sends
 *           it over USB-MIDI), played straight from flash (fm1_flash_map). With none there, COWBELL plays the
 *           starter bank compiled in (fw/app/cowbell/starter.c, from make_samples.py --starter)
 *
 * The upload, SysEx F0 7D 43 42 <cmd> <8-to-7 packed payload> F7 (7D: non-commercial; "CB"):
 *   01 INFO             -> u32 samples, u32 bank space, u32 firmware version, u32 1 if the starter bank plays
 *   02 BEGIN u32 size   the bank's size: sound stops, the bank is gone until END
 *   03 DATA u32 offset + up to 512 bytes, in order (each sector is erased as the data reaches it)
 *   04 END              checks the bank (its CRC) and plays from it
 * each answered F0 7D 43 42 <0x40 | cmd> <status> <packed u32 ...> F7 (status 0: done) over USB / BLE. */
#include <stdint.h>
#include <string.h>
#include "fm1.h"
#include "fm1_device.h"
#include "fm1_engine_control.h"
#include "instrument.h"
#include "fm_patch.h"
#include "hb_module.h"
#include "cowbell/samples.h"
typedef struct {                                /* fm1-dock's font format (tools/gen_font.py): proportional */
    uint8_t h, pad, first, last;
    const uint8_t *adv, *bw;
    const uint16_t *off;
    const uint8_t *data;
} felucca_font_t;
#include "felucca_font.h"

#define FW_VERSION 2u                          /* 2: the starter bank, 22 kHz samples (rshift) */
#define STORE_AT   0x0000u                      /* two slots of 0x2000 */
#define STORE_SLOT 0x2000u
#define BANK_AT    0x5000u
#define BANK_MAX   (536576u - BANK_AT)
#define SECTOR     FM1_FLASH_SECTOR_BYTES
#define MAX_SAMPLES 96u

extern const hb_module_t hb_module;            /* cowbell/cowbell.c */

/* ================================================================ small helpers */

static uint32_t crc32_upd(uint32_t crc, const uint8_t *p, uint32_t n)
{
    uint32_t k;
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8u; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

/* the flash, from the control task: BUSY is retried for a while */
static int fl_read(uint32_t at, void *p, uint32_t n)
{
    int r, k;
    for (k = 0; k < 200; k++) { if ((r = fm1_flash_read(at, p, n)) != FM1_FLASH_BUSY) return r; fm1_device_delay(1); }
    return FM1_FLASH_BUSY;
}
static int fl_write(uint32_t at, const void *p, uint32_t n)
{
    int r, k;
    for (k = 0; k < 200; k++) { if ((r = fm1_flash_write(at, p, n)) != FM1_FLASH_BUSY) return r; fm1_device_delay(1); }
    return FM1_FLASH_BUSY;
}
static int fl_erase(uint32_t at)
{
    int r, k;
    for (k = 0; k < 200; k++) { if ((r = fm1_flash_erase(at)) != FM1_FLASH_BUSY) return r; fm1_device_delay(1); }
    return FM1_FLASH_BUSY;
}

/* ================================================================ the sample bank */

const cb_sample_t *volatile cb_bank;
volatile uint32_t cb_bank_n;
static cb_sample_t bank_tab[MAX_SAMPLES];
static uint32_t starter;                        /* 1: playing the compiled-in starter bank */

/* the bank in flash (format: tools/make_samples.py write_bank) -> bank_tab; 0: playing from it */
static int bank_open(void)
{
    uint8_t h[16];
    const uint8_t *m;
    uint32_t n, size, i;
    cb_bank_n = 0;
    if (fl_read(BANK_AT, h, sizeof h) || memcmp(h, "CBNK", 4) || rd16(h + 4) != 1u)
        return -1;
    n = rd16(h + 6);
    size = rd32(h + 8);
    if (!n || n > MAX_SAMPLES || size > BANK_MAX || size < 16u + 24u * n)
        return -2;
    m = fm1_flash_map(BANK_AT, size);
    if (!m || crc32_upd(0, m + 16, size - 16u) != rd32(h + 12))
        return -3;
    for (i = 0; i < n; i++) {
        const uint8_t *e = m + 16 + 24u * i;
        uint32_t frames = rd32(e + 16), off = rd32(e + 20);
        cb_sample_t *s = &bank_tab[i];
        if (e[13] != 0 || e[14] > 1u || off > size || (frames + 1u) / 2u > size - off)   /* IMA ADPCM, a rate, inside */
            return -4;
        memcpy(s->name, e, 10);
        s->name[9] = 0;
        s->root = e[10]; s->gain = e[11]; s->index0 = e[12]; s->rshift = e[14];
        s->n = frames;
        s->data = m + off;
    }
    cb_bank = bank_tab;
    cb_bank_n = n;                              /* last: render() sees a whole table */
    starter = 0;
    return 0;
}

/* no bank in flash: the starter bank compiled in; 0: playing from it */
static int starter_open(void)
{
    cb_bank_n = 0;
    if (!cb_starter_n)
        return -1;
    cb_bank = cb_starter;
    cb_bank_n = cb_starter_n;
    starter = 1;
    return 0;
}

/* ================================================================ the pattern store */

#define ST_MAGIC 0x54534243u                    /* "CBST" */
static int st_newest(uint32_t n, uint32_t *slot, uint32_t *gen)
{
    uint32_t k, h[4], best = 0xFFFFFFFFu, bg = 0;
    for (k = 0; k < 2u; k++) {
        if (fl_read(STORE_AT + k * STORE_SLOT, h, sizeof h) || h[0] != ST_MAGIC || (n && h[2] != n) ||
            h[2] > STORE_SLOT - 16u)
            continue;
        if (best == 0xFFFFFFFFu || h[1] > bg) { best = k; bg = h[1]; }
    }
    *slot = best; *gen = bg;
    return best == 0xFFFFFFFFu ? -1 : 0;
}
static int h_bank_load(void *p, uint32_t n)
{
    uint32_t slot, gen, h[4], k;
    for (k = 0; k < 2u; k++) {                  /* the newest valid one; if it is damaged, the other */
        if (st_newest(n, &slot, &gen))
            return -1;
        if (k) slot ^= 1u;
        if (fl_read(STORE_AT + slot * STORE_SLOT, h, sizeof h) || h[0] != ST_MAGIC || h[2] != n)
            continue;
        if (!fl_read(STORE_AT + slot * STORE_SLOT + 16u, p, n) && crc32_upd(0, p, n) == h[3])
            return 0;
    }
    return -1;
}
static int h_bank_save(const void *p, uint32_t n)
{
    uint32_t slot, gen = 0, h[4], at, s;
    if (n > STORE_SLOT - 16u)
        return -1;
    slot = st_newest(n, &slot, &gen) ? 0u : slot ^ 1u;   /* the older slot: the newer stays good */
    at = STORE_AT + slot * STORE_SLOT;
    for (s = 0; s < STORE_SLOT; s += SECTOR)
        if (fl_erase(at + s)) return -2;
    h[0] = ST_MAGIC; h[1] = gen + 1u; h[2] = n; h[3] = crc32_upd(0, p, n);
    if (fl_write(at + 16u, p, n)) return -3;
    return fl_write(at, h, sizeof h) ? -4 : 0;  /* the header last: a cut save stays invalid */
}

/* ================================================================ the screen */

static void h_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > 240) w = 240 - x;
    if (y + h > 240) h = 240 - y;
    if (w > 0 && h > 0)
        fm1_screen_fill((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, c);
}
/* the module hands its pixels high byte first (fm1-dock's panel order): back to native, in place */
static void h_blit(int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t *px)
{
    uint16_t *q = (uint16_t *)px;
    uint32_t i, n = (uint32_t)(w * h);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 240 || y + h > 240)
        return;
    for (i = 0; i < n; i++) q[i] = (uint16_t)(q[i] >> 8 | q[i] << 8);
    fm1_screen_blit((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, q, n);
}

/* a small canvas for text on a ground (fm1-dock's gfx.c, native pixel order) */
#define CV_MAX (240u * 32u)
static uint16_t cv_px[CV_MAX];
static uint32_t cv_w, cv_h;
static void h_cv_begin(int32_t w, int32_t h, uint16_t bg)
{
    uint32_t i;
    if (w < 1) w = 1;
    if (w > 240) w = 240;
    if (h < 1) h = 1;
    if ((uint32_t)(w * h) > CV_MAX) h = (int32_t)(CV_MAX / (uint32_t)w);
    cv_w = (uint32_t)w; cv_h = (uint32_t)h;
    for (i = 0; i < cv_w * cv_h; i++) cv_px[i] = bg;
}
static void cv_pset(int32_t x, int32_t y, uint16_t c)
{
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h) cv_px[(uint32_t)y * cv_w + (uint32_t)x] = c;
}
static void h_cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) cv_pset(i, j, c);
}
static void h_cv_pixel(int32_t x, int32_t y, uint16_t c) { cv_pset(x, y, c); }
static uint32_t glyph(const felucca_font_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a') ch -= 32u;
    if (ch < f->first || ch > f->last) ch = '?';
    return ch - f->first;
}
static void h_cv_text(int32_t x, int32_t y, uint32_t big, const char *s, uint16_t c)
{
    const felucca_font_t *f = big ? &FONT_L : &FONT_S;
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 7u) / 8u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++)
                if (gd[gy * bpr + gx / 8u] >> (7u - (gx & 7u)) & 1u)
                    cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, c);
        x += f->adv[gi];
    }
}
static int32_t h_text_w(uint32_t big, const char *s)
{
    const felucca_font_t *f = big ? &FONT_L : &FONT_S;
    int32_t w = 0;
    for (; *s; s++) w += f->adv[glyph(f, (uint8_t)*s)];
    return w;
}
static void h_cv_blit(int32_t x, int32_t y)
{
    if (x >= 0 && y >= 0 && x + (int32_t)cv_w <= 240 && y + (int32_t)cv_h <= 240)
        fm1_screen_blit((unsigned)x, (unsigned)y, cv_w, cv_h, cv_px, cv_w * cv_h);
}
static void h_text(int32_t x, int32_t y, int32_t w, uint32_t big, const char *s, uint16_t fg, uint32_t center)
{
    int32_t tw = h_text_w(big, s);
    h_cv_begin(w, big ? 32 : 16, 0);
    h_cv_text(center && tw < w ? (w - tw) / 2 : 0, 0, big, s, fg);
    h_cv_blit(x, y);
}

/* ================================================================ LEDs: 0 off, anything else on (as fm1-dock) */

static uint8_t led_want[41], led_is[41];
static void h_led(uint32_t id, uint32_t on) { if (id < 41u) led_want[id] = (uint8_t)(on != 0); }
static void leds_update(uint32_t now)
{
    uint32_t i;
    (void)now;
    for (i = 0; i < 41u; i++) {
        uint8_t v = led_want[i];
        if (v != led_is[i]) { led_is[i] = v; fm1_led_set(i, v); }
    }
}

/* ================================================================ the rest of the host */

static uint32_t ev_ticks, in_midi;
static uint32_t h_ms(void) { return fm1_millis(); }
static uint32_t h_ticks(void) { return in_midi ? ev_ticks : fm1_device_ticks(); }   /* a MIDI byte's arrival */
static void h_log(const char *s) { (void)s; }
static void h_log_u32(const char *s, uint32_t v) { (void)s; (void)v; }
static uint32_t rnd = 0x1234567u;
static uint32_t h_rand(void) { rnd = rnd * 1664525u + 1013904223u; return rnd; }
static int h_load(void *p, uint32_t n) { (void)p; (void)n; return -1; }
static void h_save(const void *p, uint32_t n) { (void)p; (void)n; }
static void h_midi_send(uint32_t st, uint32_t d1, uint32_t d2)
{
    uint8_t m[3] = {(uint8_t)st, (uint8_t)(d1 & 127u), (uint8_t)(d2 & 127u)};
    uint32_t k = st & 0xF0u, n = st >= 0xF8u ? 1u : (k == 0xC0u || k == 0xD0u) ? 2u : 3u;
    (void)fm1_midi_send(m, n);
}
static uint32_t h_master(void) { return 4096u; }

static const hb_host_t HOST = {
    .abi = HB_ABI, .size = sizeof(hb_host_t),
    .ms = h_ms, .ticks = h_ticks, .log = h_log, .log_u32 = h_log_u32,
    .fill = h_fill, .text = h_text,
    .cv_begin = h_cv_begin, .cv_rect = h_cv_rect, .cv_pixel = h_cv_pixel, .cv_text = h_cv_text, .cv_blit = h_cv_blit,
    .led = h_led, .rand = h_rand, .load = h_load, .save = h_save, .midi_send = h_midi_send, .master = h_master,
    .blit = h_blit, .text_w = h_text_w, .bank_load = h_bank_load, .bank_save = h_bank_save,
};

/* ================================================================ MIDI in: routing task -> control task */

#define MQ 64u
static struct { uint32_t t; uint8_t m[3], n; } mq[MQ];
static volatile uint32_t mq_w, mq_r;
void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n)
{
    uint32_t w = mq_w, i;
    (void)source;
    if (!m || !n || n > 3u || w - mq_r >= MQ) return;
    mq[w % MQ].t = fm1_device_ticks();
    for (i = 0; i < 3u; i++) mq[w % MQ].m[i] = i < n ? m[i] : 0;
    mq[w % MQ].n = (uint8_t)n;
    __asm__ volatile("" ::: "memory");
    mq_w = w + 1u;
}

/* SysEx (the upload): routing task -> control task, whole frames */
#define SQ 4u
#define SX_MAX 1024u
static uint8_t sq[SQ][SX_MAX];
static uint16_t sq_n[SQ];
static volatile uint32_t sq_w, sq_r;
void fm1_app_sysex(const uint8_t *m, size_t n)
{
    uint32_t w = sq_w;
    if (!m || n < 6u || n > SX_MAX || m[1] != 0x7Du || m[2] != 0x43u || m[3] != 0x42u || w - sq_r >= SQ) return;
    memcpy(sq[w % SQ], m, n);
    sq_n[w % SQ] = (uint16_t)n;
    __asm__ volatile("" ::: "memory");
    sq_w = w + 1u;
}

/* ================================================================ the upload */

static uint32_t unpack7(const uint8_t *in, uint32_t n, uint8_t *out, uint32_t max)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t hi = in[i++], k;
        for (k = 0; k < 7u && i < n && o < max; k++) out[o++] = (uint8_t)(in[i++] | ((hi >> k & 1u) << 7));
    }
    return o;
}
static uint32_t pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t k, hi = o++;
        out[hi] = 0;
        for (k = 0; k < 7u && i < n; k++, i++) {
            out[hi] |= (uint8_t)((in[i] >> 7) << k);
            out[o++] = in[i] & 0x7Fu;
        }
    }
    return o;
}
static void reply(uint32_t cmd, uint32_t status, const uint32_t *v, uint32_t nv)
{
    uint8_t raw[16], m[40];
    uint32_t i, n = 0;
    for (i = 0; i < nv && i < 4u; i++) { raw[4 * i] = (uint8_t)v[i]; raw[4 * i + 1] = (uint8_t)(v[i] >> 8);
                                         raw[4 * i + 2] = (uint8_t)(v[i] >> 16); raw[4 * i + 3] = (uint8_t)(v[i] >> 24); }
    m[n++] = 0xF0; m[n++] = 0x7D; m[n++] = 0x43; m[n++] = 0x42;
    m[n++] = (uint8_t)(0x40u | cmd); m[n++] = (uint8_t)(status & 0x7Fu);
    n += pack7(raw, 4u * nv, m + n);
    m[n++] = 0xF7;
    for (i = 0; i < 50u && fm1_midi_send(m, n) == -2; i++) fm1_device_delay(1);
}

static uint8_t state;                           /* 0 starting, 1 no bank, 2 playing */
static uint32_t up_size, up_next, up_erased, up_active;
static uint8_t up_buf[600];

static void screen_message(const char *l1, const char *l2, const char *l3)
{
    h_fill(0, 0, 240, 240, HB_RGB(236, 150, 180));
    h_fill(0, 0, 240, 20, HB_RGB(210, 80, 120));
    h_cv_begin(232, 16, HB_RGB(210, 80, 120)); h_cv_text(0, 0, 0, "COWBELL", 0xFFFFu); h_cv_blit(4, 2);
    h_cv_begin(240, 16, HB_RGB(236, 150, 180)); h_cv_text(8, 0, 0, l1, HB_RGB(75, 21, 40)); h_cv_blit(0, 90);
    h_cv_begin(240, 16, HB_RGB(236, 150, 180)); h_cv_text(8, 0, 0, l2, HB_RGB(75, 21, 40)); h_cv_blit(0, 112);
    h_cv_begin(240, 16, HB_RGB(236, 150, 180)); h_cv_text(8, 0, 0, l3, HB_RGB(75, 21, 40)); h_cv_blit(0, 134);
}

static void sysex(const uint8_t *m, uint32_t n)
{
    uint32_t cmd = m[4], len = unpack7(m + 5, n - 6u, up_buf, sizeof up_buf), v[4];
    switch (cmd) {
    case 0x01:
        v[0] = cb_bank_n; v[1] = BANK_MAX; v[2] = FW_VERSION; v[3] = starter;
        reply(cmd, 0, v, 4);
        break;
    case 0x02:
        if (len < 4u || rd32(up_buf) > BANK_MAX || rd32(up_buf) < 16u) { reply(cmd, 1, 0, 0); break; }
        if (state == 2u) hb_module.release();
        cb_bank_n = 0;
        up_size = rd32(up_buf); up_next = up_erased = 0; up_active = 1;
        screen_message("RECEIVING SAMPLES", "KEEP THE USB CABLE IN", "");
        reply(cmd, 0, 0, 0);
        break;
    case 0x03: {
        uint32_t off, dn;
        if (!up_active || len < 4u || (off = rd32(up_buf)) != up_next || (dn = len - 4u) > up_size - off) {
            v[0] = up_next; reply(cmd, 1, v, 1); break;
        }
        while (up_erased < off + dn) {          /* each sector erased as the data reaches it */
            if (fl_erase(BANK_AT + up_erased)) { v[0] = up_next; reply(cmd, 2, v, 1); return; }
            up_erased += SECTOR;
        }
        if (fl_write(BANK_AT + off, up_buf + 4, dn)) { v[0] = up_next; reply(cmd, 3, v, 1); break; }
        up_next = off + dn;
        v[0] = up_next;
        reply(cmd, 0, v, 1);
        if ((off / 16384u) != (up_next / 16384u)) {   /* the progress, now and then */
            char t[24] = "RECEIVING   %";
            uint32_t pc = up_next * 100u / up_size;
            t[10] = (char)('0' + pc / 100u % 10u); t[11] = (char)('0' + pc / 10u % 10u); t[12] = (char)('0' + pc % 10u);
            if (pc < 100u) t[10] = ' ';
            screen_message(t, "KEEP THE USB CABLE IN", "");
        }
        break;
    }
    case 0x04: {
        int r = up_active && up_next == up_size ? bank_open() : -9;
        up_active = 0;
        v[0] = cb_bank_n;
        reply(cmd, r ? (uint32_t)(10 - r) : 0u, v, 1);
        if (!r) {
            if (state != 2u) { hb_module.init(&HOST); state = 2; }
            hb_module.draw(1);
        } else if (starter_open() == 0) {     /* a broken upload: back to the starter bank */
            if (state != 2u) { hb_module.init(&HOST); state = 2; }
            hb_module.draw(1);
        } else {
            state = 1;
            screen_message("THE SAMPLES DID NOT ARRIVE", "WHOLE: SEND THEM AGAIN", "(tools/cowbell_upload.py)");
        }
        break;
    }
    default:
        reply(cmd, 0x7F, 0, 0);
    }
}

/* ================================================================ the SDK's application hooks */

static uint32_t prev_keys, prev_buttons;
volatile uint32_t cb_dbg[8];
#ifdef CB_DEBUG
#define MARK(k) fm1_screen_fill(6u * (k), 204, 5, 8, 0x07E0u)
#else
#define MARK(k) ((void)0)
#endif                    /* debugging: control calls, keys, buttons, blocks, state */

void groove_screen_init(void)
{
    fm1_controls_init();                        /* the panel scan (board/panel.c) and the screen: the app starts them */
    fm1_screen_init();
    state = 0;
}

void groove_screen_control(void)
{
    struct fm1_controls c;
    uint32_t now = fm1_millis(), k, ch;
    fm1_controls_read(&c);
    cb_dbg[0]++; cb_dbg[1] |= c.keys; cb_dbg[2] |= c.buttons; cb_dbg[4] = state; cb_dbg[5] += (uint32_t)(c.encoders[0] != 0);
    (void)groove_master_update(c.knob);         /* MASTER: the output level the mixer applies */

    while (sq_r != sq_w) {                      /* uploads first: they own the screen while they run */
        sysex(sq[sq_r % SQ], sq_n[sq_r % SQ]);
        sq_r++;
    }
    if (state == 0u) {                          /* the first pass: the bank, then the module */
        if (bank_open() == 0 || starter_open() == 0) {
            MARK(0);
            hb_module.init(&HOST);
            MARK(1);
            state = 2;
            hb_module.draw(1);
            MARK(2);
#ifdef CB_DEBUG
            {   /* the time this first pass finished (a restart loop shows a small number) */
                char t[16] = "UP      MS"; uint32_t v = fm1_millis(), i;
                for (i = 0; i < 5u; i++) { t[7 - i] = (char)('0' + v % 10u); v /= 10u; }
                h_cv_begin(120, 16, 0); h_cv_text(0, 0, 0, t, 0xFFFFu); h_cv_blit(120, 200);
            }
#endif
        } else {
            state = 1;
            screen_message("NO SAMPLES YET", "PLUG IN USB AND RUN", "tools/cowbell_upload.py");
        }
    }
    if (state != 2u || up_active) {
        mq_r = mq_w;                            /* nothing to play yet */
        prev_keys = c.keys; prev_buttons = c.buttons;
        return;
    }

    MARK(3);
    ch = c.buttons ^ prev_buttons;              /* the panel, as events (fm1-dock's ids are the SDK's) */
    for (k = 0; k < 14u; k++)
        if (ch >> k & 1u)
            hb_module.ui((c.buttons >> k & 1u ? HB_EV_BTN_DOWN : HB_EV_BTN_UP) | k, 0);
    ch = c.keys ^ prev_keys;
    for (k = 0; k < 27u; k++)
        if (ch >> k & 1u)
            hb_module.ui((c.keys >> k & 1u ? HB_EV_KEY_DOWN : HB_EV_KEY_UP) | k, 0);
    for (k = 0; k < 7u; k++)
        if (c.encoders[k])
            hb_module.ui(HB_EV_ENC | k, c.encoders[k]);
    prev_keys = c.keys; prev_buttons = c.buttons;

    while (mq_r != mq_w) {                      /* MIDI in, with each byte's arrival time */
        uint32_t i = mq_r % MQ;
        ev_ticks = mq[i].t; in_midi = 1;
        hb_module.midi(mq[i].m[0], mq[i].m[1], mq[i].m[2]);
        in_midi = 0;
        mq_r++;
    }
    MARK(4);
    hb_module.draw(0);
    MARK(5);
    leds_update(now);
    MARK(6);
#ifdef CB_DEBUG
    {   /* the counters, bottom left: control calls, keys / buttons seen, audio blocks */
        static uint32_t last;
        if (now - last > 300u) {
            char t[40]; uint32_t i, v[4] = {cb_dbg[0], cb_dbg[1], cb_dbg[2], cb_dbg[3]}; char *q = t;
            last = now;
            for (i = 0; i < 4u; i++) { int sh; *q++ = "CKBA"[i]; for (sh = 20; sh >= 0; sh -= 4) *q++ = "0123456789ABCDEF"[v[i] >> sh & 15u]; *q++ = ' '; }
            *q = 0;
            h_cv_begin(240, 16, 0); h_cv_text(0, 0, 0, t, 0xFFFFu); h_cv_blit(0, 222);
        }
    }
#endif
}

/* the audio interrupt (board/audio.c): one 128-frame block, signed Q23 */
static int32_t blk[FM1_BLOCK_WORDS];
FM1_AUDIO_CODE int cowbell_block(int32_t *out)
{
    uint32_t i;
    cb_dbg[3]++;
    for (i = 0; i < FM1_BLOCK_WORDS; i++) { blk[i] = 0; __asm__ volatile("" ::: "memory"); }
    if (state == 2u && cb_bank_n)
        hb_module.render(blk, FM1_BLOCK_FRAMES);
    for (i = 0; i < FM1_BLOCK_WORDS; i++) out[i] = blk[i] * 256;   /* Q15 -> Q23 */
    return 0;
}

/* the groovebox's hooks: COWBELL has none of its projects, sample slots, DX7 voices or tracks */
static struct groove_project scratch;
void groove_project_init(void) {}
void groove_project_poll(void) {}
struct groove_project *groove_project_scratch(void) { return &scratch; }
void groove_project_metadata(const struct groove_project *p) { (void)p; }
void groove_project_name(char name[13]) { memcpy(name, "COWBELL     ", 13); }
void groove_samples_init(void) {}
void groove_samples_poll(void) {}
void groove_transfer_init(void) {}
void groove_transfer_poll(void) {}
void groove_fm_library_init(void) {}
FM1_DSP_CODE void groove_fm_program(uint8_t p[156], unsigned program)
{
    unsigned i;
    (void)program;
    for (i = 0; i < 156u; i++) { p[i] = 0; __asm__ volatile("" ::: "memory"); }
}
void groove_midi_init(void) {}
void groove_midi_clock_reset(void) {}
void groove_midi_poll(void) {}
void groove_midi_panic(void) {}
FM1_DSP_CODE unsigned groove_midi_mode(unsigned track) { (void)track; return 0; }
FM1_DSP_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity) { (void)track; (void)note; (void)velocity; }
FM1_DSP_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_DSP_CODE void groove_midi_transport(unsigned owner, uint8_t status) { (void)owner; (void)status; }
FM1_DSP_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner; (void)tempo; }
