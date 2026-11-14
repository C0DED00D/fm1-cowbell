/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 firmware build: cowbell_host.c reads the bank from the flash's user partition (put there by
 * tools/cowbell_upload.py or the web uploader) and points cb_bank at it; with none there, at the starter bank
 * compiled in (starter.c, a smaller 22 kHz copy: the whole bank does not fit the image). Same sample record as
 * cowbell/samples.h. */
#ifndef CB_SAMPLES_H
#define CB_SAMPLES_H
#include <stdint.h>
typedef struct {
    char name[10];
    uint8_t root, gain /* Q7 */, index0, rshift /* 44.1 kHz >> rshift */;
    uint32_t n;
    const uint8_t *data;
} cb_sample_t;
extern const cb_sample_t *volatile cb_bank;
extern volatile uint32_t cb_bank_n;
extern const cb_sample_t cb_starter[];
extern const uint32_t cb_starter_n;
#define CB_SAMPLES cb_bank
#define CB_N_SAMPLES cb_bank_n
#endif
