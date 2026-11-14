#!/usr/bin/env python3
"""Synthesize the COWBELL sample bank and write it as 4-bit IMA ADPCM C data.

Writes cowbell/samples.c (the format is fixed by cowbell/samples.h) and preview WAVs, decoded from the
ADPCM exactly as the device plays them, to out/samples/. Every sound is synthesized here (no recordings),
so the bank has no licence questions. Needs only numpy.

    python tools/make_samples.py                 # the built-in bank
    python tools/make_samples.py --wav my_bells  # plus every .wav in my_bells/, appended
    python tools/make_samples.py --wav my_bells --no-builtin   # only your own
"""
import argparse
import math
import os
import re
import struct
import sys
import wave

import numpy as np

FS = 44100
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, "cowbell", "samples.c")
OUT_WAV = os.path.join(ROOT, "out", "samples")
BUDGET = 230 * 1024          # aim
HARD_MAX = 240 * 1024        # refuse to write more than this
PEAK = 10 ** (-1 / 20)       # stored audio is normalized to -1 dBFS
MIN_SNR = 20.0               # dB, decoded ADPCM vs the source

# ---------------------------------------------------------------------------------------------------------
# IMA ADPCM (standard tables)

STEPS = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767]
ADJ = [-1, -1, -1, -1, 2, 4, 6, 8]


# DELTA[index][m]: the decoder's step for magnitude bits m (0..7) at step index `index`
DELTA = [[(st >> 3) + (st if m & 4 else 0) + (st >> 1 if m & 2 else 0) + (st >> 2 if m & 1 else 0)
          for m in range(8)] for st in STEPS]


def adpcm_encode(pcm, index0, lookahead=True):
    """Closed-loop IMA encoder: every choice is made against the decoder's own reconstruction (pred, index).

    With lookahead, each of the 8 magnitudes is scored by its own squared error plus the best error it leaves
    for the next sample (the step index it leads to matters), which gains several dB on bright sounds over
    the textbook quantizer. Any standard IMA decoder plays the result. Returns the nibbles."""
    pred, index = 0, index0
    codes = []
    n = len(pcm)
    for i in range(n):
        target = pcm[i]
        sign = 8 if target < pred else 0
        row = DELTA[index]
        nxt = pcm[i + 1] if lookahead and i + 1 < n else None
        best_e, best_m, best_p = None, 0, pred
        for m in range(8):
            p = pred - row[m] if sign else pred + row[m]
            p = -32768 if p < -32768 else 32767 if p > 32767 else p
            e = (p - target) ** 2
            if nxt is not None:
                ni = index + ADJ[m]
                ni = 0 if ni < 0 else 88 if ni > 88 else ni
                d = abs(nxt - p)
                e += min((dd - d) ** 2 for dd in DELTA[ni])      # ignores clamping: fine for a score
            if best_e is None or e < best_e:
                best_e, best_m, best_p = e, m, p
        pred = best_p
        index += ADJ[best_m]
        index = 0 if index < 0 else 88 if index > 88 else index
        codes.append(sign | best_m)
    return codes


def adpcm_decode(data, n, index0):
    """Reference decoder (what the C player does): bytes, LOW nibble first."""
    pred, index = 0, index0
    out = np.zeros(n, dtype=np.int16)
    for i in range(n):
        b = (data[i >> 1] >> (4 * (i & 1))) & 15
        step = STEPS[index]
        diff = step >> 3
        if b & 4: diff += step
        if b & 2: diff += step >> 1
        if b & 1: diff += step >> 2
        if b & 8: diff = -diff
        pred += diff
        pred = -32768 if pred < -32768 else 32767 if pred > 32767 else pred
        index += ADJ[b & 7]
        index = 0 if index < 0 else 88 if index > 88 else index
        out[i] = pred
    return out


def pack(codes):
    if len(codes) & 1:
        codes = codes + [0]
    return bytes(codes[i] | (codes[i + 1] << 4) for i in range(0, len(codes), 2))


def snr_db(ref, test):
    ref = np.asarray(ref, dtype=np.float64); err = np.asarray(test, dtype=np.float64) - ref
    return 10 * math.log10(np.sum(ref ** 2) / max(np.sum(err ** 2), 1e-9))


def encode_best(pcm):
    """Pick the starting step index by trying them on the attack, then encode the whole sample."""
    head = pcm[:1024]
    best = min(range(0, 89, 2), key=lambda i0: -snr_db(
        head, adpcm_decode(pack(adpcm_encode(head, i0, lookahead=False)), len(head), i0)))
    data = pack(adpcm_encode(pcm, best))
    return best, data


# ---------------------------------------------------------------------------------------------------------
# DSP helpers

def tt(sec):
    return np.arange(int(sec * FS)) / FS


def biquad(x, kind, f, q=0.707):
    """RBJ cookbook biquad: kind = 'lp', 'hp' or 'bp' (0 dB peak)."""
    w = 2 * math.pi * f / FS
    c, s = math.cos(w), math.sin(w)
    al = s / (2 * q)
    if kind == "lp":
        b = [(1 - c) / 2, 1 - c, (1 - c) / 2]
    elif kind == "hp":
        b = [(1 + c) / 2, -(1 + c), (1 + c) / 2]
    else:
        b = [al, 0.0, -al]
    a0, a1, a2 = 1 + al, -2 * c, 1 - al
    b0, b1, b2 = b[0] / a0, b[1] / a0, b[2] / a0
    a1 /= a0; a2 /= a0
    y = [0.0] * len(x)
    x1 = x2 = y1 = y2 = 0.0
    for i, v in enumerate(x.tolist()):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return np.array(y)


def square(f, t):
    """Band-limited square wave (odd harmonics below 20 kHz), starts at 0."""
    out = np.zeros_like(t)
    k = 1
    while k * f < 12000:
        out += np.sin(2 * math.pi * k * f * t) / k
        k += 2
    return out * (4 / math.pi)


def modes(t, f0, ratios, amps, taus, detune=0.0015, rng=None):
    """Sum of damped sine partials. Each partial is a slightly split doublet (a real bell is not round),
    which gives the slow beating of a struck cowbell."""
    out = np.zeros_like(t)
    for j, (r, a, tau) in enumerate(zip(ratios, amps, taus)):
        f = f0 * r
        if f > 20000:
            continue
        d = detune * (1 + 0.5 * math.sin(j * 2.3))
        env = np.exp(-t / tau)
        out += a * env * (0.6 * np.sin(2 * math.pi * f * (1 - d) * t) + 0.4 * np.sin(2 * math.pi * f * (1 + d) * t))
    return out


def strike(n, rng, fc, q=0.8, tau=0.002, length=0.012, click=0.0):
    """A stick hitting metal or wood: a short burst of band-passed noise, plus an optional click."""
    m = int(length * FS)
    t = np.arange(m) / FS
    burst = biquad(rng.standard_normal(m) * np.exp(-t / tau), "bp", fc, q)
    burst /= max(np.max(np.abs(burst)), 1e-9)
    out = np.zeros(n)
    out[:m] = burst
    if click:
        out[:int(0.0008 * FS)] += click * np.hanning(int(0.0008 * FS))
    return out


def comb(x, d_sec, g):
    """Feedback comb y[n] = x[n] + g y[n-D], done a block of D at a time."""
    d = int(d_sec * FS)
    y = x.copy()
    for k in range(d, len(y), d):
        y[k:k + d] += g * y[k - d:k][:len(y[k:k + d])]
    return y


def delay(x, d_sec):
    d = int(d_sec * FS)
    return np.concatenate([np.zeros(d), x[:len(x) - d]])


def fm(t, fc, ratio, i0, tau_i, tau_a):
    """2-operator FM (phase modulation): the modulation index decays, so the bell darkens as it rings."""
    index = i0 * np.exp(-t / tau_i)
    return np.exp(-t / tau_a) * np.sin(2 * math.pi * fc * t + index * np.sin(2 * math.pi * fc * ratio * t))


def midi(f):
    return int(round(69 + 12 * math.log2(f / 440)))


# ---------------------------------------------------------------------------------------------------------
# The sounds. Each returns (signal, root note). Durations are generous: trim() ends each at -60 dB.

def s808(f1=540, f2=800, bp=2640, fast=0.010, slow=0.075, mix=0.6, sec=0.6):
    t = tt(sec)
    x = square(f1, t) + square(f2, t)
    # the 808 band-passes the pair; keep some of the dry pair so the tones stay heard
    y = biquad(x, "bp", bp, 1.0) + 0.35 * biquad(x, "lp", 3000)
    y = biquad(biquad(y, "lp", 7000), "lp", 9000)                # tame the top: 4-bit ADPCM likes it smooth
    env = mix * np.exp(-t / fast) + (1 - mix) * np.exp(-t / slow)   # two-stage: fast, then slow
    return y * env, midi(f1)


def scr78(f1=785, f2=1180, tau=0.028, sec=0.3, seed=1):
    t = tt(sec)
    rng = np.random.default_rng(seed)
    x = square(f1, t) + 0.8 * square(f2, t)
    x = biquad(x, "bp", 2000, 0.7) * np.exp(-t / tau)
    x = x / np.max(np.abs(x)) + 0.9 * strike(len(t), rng, 5000, 0.7, 0.0008, 0.006, click=0.6)
    return biquad(x, "lp", 8000), midi(f1)


# a struck cowbell's partial ratios, levels and ring times (seconds)
BELL_R = [1.0, 1.48, 2.0, 2.66, 3.2, 4.1, 5.3]
BELL_A = [1.0, 0.75, 0.35, 0.5, 0.28, 0.16, 0.08]
BELL_T = [0.11, 0.085, 0.06, 0.055, 0.035, 0.025, 0.018]


def real(f0=560, damp=1.0, bright=1.0, hit=0.35, sec=0.9, seed=2, strike_fc=3500):
    t = tt(sec)
    rng = np.random.default_rng(seed)
    amps = [a * (bright ** j) for j, a in enumerate(BELL_A)]
    taus = [tau * damp * (1 - 0.08 * j * (1 - damp)) for j, tau in enumerate(BELL_T)]
    x = modes(t, f0, BELL_R, amps, taus, rng=rng)
    x /= np.max(np.abs(x))
    x += hit * strike(len(t), rng, strike_fc, 0.9, 0.0015, 0.010, click=0.3 * hit)
    return x, midi(f0)


def big(f0=520, sec=1.0, seed=3):
    """'More cowbell': two slightly detuned hits, early reflections, a short room tail, saturation."""
    a, _ = real(f0, damp=1.3, bright=1.1, hit=0.4, sec=sec, seed=seed)
    b, _ = real(f0 * 1.009, damp=1.25, bright=1.0, hit=0.3, sec=sec, seed=seed + 1)
    x = a + 0.8 * delay(b, 0.004)
    x /= np.max(np.abs(x))
    dark = biquad(x, "lp", 4000)
    er = sum(g * delay(dark, d) for d, g in [(0.007, 0.5), (0.011, -0.4), (0.017, 0.35), (0.023, 0.3),
                                              (0.031, -0.25), (0.041, 0.2)])
    tail = sum(comb(0.25 * dark, d, 0.62) for d in (0.0297, 0.0371, 0.0411, 0.0437))
    tail *= np.exp(-tt(sec) / 0.18)
    y = x + 0.6 * er + 0.3 * tail
    y /= np.max(np.abs(y))
    return np.tanh(2.2 * y) / math.tanh(2.2), midi(f0)


def agogo(f0, tau=0.16, sec=0.8, seed=4):
    t = tt(sec)
    rng = np.random.default_rng(seed)
    x = modes(t, f0, [1.0, 2.21, 2.89, 3.92, 5.41], [1.0, 0.32, 0.12, 0.14, 0.05],
              [tau, tau * 0.45, tau * 0.3, tau * 0.25, tau * 0.15], detune=0.0008)
    x /= np.max(np.abs(x))
    return x + 0.25 * strike(len(t), rng, 6000, 1.0, 0.001, 0.006), midi(f0)


def tiny(f0=1900, sec=0.4, seed=5):
    """A goat bell: high, short, with the clapper bouncing back for a second, softer hit."""
    t = tt(sec)
    rng = np.random.default_rng(seed)
    one = modes(t, f0, [1.0, 1.59, 2.36, 3.12], [1.0, 0.6, 0.35, 0.2], [0.07, 0.05, 0.035, 0.025])
    one /= np.max(np.abs(one))
    one += 0.3 * strike(len(t), rng, 7000, 1.0, 0.0006, 0.004)
    return one + 0.45 * delay(one, 0.038), midi(f0)


def clank(f0=380, sec=0.4, seed=6):
    """A dull iron bell: low, heavily damped, a lot of hit noise."""
    t = tt(sec)
    rng = np.random.default_rng(seed)
    x = modes(t, f0, [1.0, 1.31, 1.9, 2.47, 3.3, 4.4], [1.0, 0.8, 0.6, 0.45, 0.3, 0.2],
              [0.06, 0.045, 0.035, 0.03, 0.02, 0.015], detune=0.004)
    x /= np.max(np.abs(x))
    noise = biquad(rng.standard_normal(len(t)) * np.exp(-t / 0.025), "bp", 1500, 0.6)
    noise /= np.max(np.abs(noise))
    return x + 0.6 * noise, midi(f0)


def rusty(f0=470, sec=0.7, seed=7):
    """Detuned partials beating fast, plus an asymmetric 'loose rattle' nonlinearity: buzzy."""
    t = tt(sec)
    rng = np.random.default_rng(seed)
    x = np.zeros_like(t)
    for r, a, tau, dhz in zip([1.0, 1.47, 2.03, 2.71, 3.3], [1.0, 0.8, 0.5, 0.45, 0.25],
                              [0.12, 0.09, 0.07, 0.05, 0.035], [9, 14, 21, 17, 26]):
        e = a * np.exp(-t / tau)
        x += e * (np.sin(2 * math.pi * (f0 * r - dhz / 2) * t) + np.sin(2 * math.pi * (f0 * r + dhz / 2) * t))
    x /= np.max(np.abs(x))
    buzz = np.tanh(5 * (x + 0.2)) - math.tanh(1.0)
    rattle = biquad(rng.standard_normal(len(t)), "bp", 3200, 1.5) * np.maximum(x, 0)
    y = x + 0.25 * buzz * np.exp(-t / 0.1) + 0.8 * rattle
    return y + 0.3 * strike(len(t), rng, 3000, 0.8, 0.0015, 0.01), midi(f0)


def tin(f0=950, sec=0.4, seed=8):
    """A thin sheet-metal bell: many crowded, quickly dying partials."""
    t = tt(sec)
    rng = np.random.default_rng(seed)
    ratios = np.concatenate([[1.0], np.sort(rng.uniform(1.2, 5.5, 9))])
    amps = [1.0] + list(0.7 / ratios[1:] ** 0.5)
    taus = [0.09] + list(0.07 / ratios[1:] ** 0.6)
    x = modes(t, f0, ratios, amps, taus, detune=0.003)
    x /= np.max(np.abs(x))
    return x + 0.4 * strike(len(t), rng, 5000, 0.8, 0.001, 0.008), midi(f0)


def block(f0=1050, sec=0.2, seed=9):
    """A wood block: one strong, fast-dying resonance, a weaker upper one, a woody knock."""
    t = tt(sec)
    rng = np.random.default_rng(seed)
    x = modes(t, f0, [1.0, 2.44, 3.6], [1.0, 0.3, 0.1], [0.022, 0.010, 0.006], detune=0.0)
    x /= np.max(np.abs(x))
    return x + 0.5 * strike(len(t), rng, 2500, 1.2, 0.0008, 0.005), midi(f0)


def clave(f0=2450, sec=0.25, seed=10):
    t = tt(sec)
    rng = np.random.default_rng(seed)
    x = modes(t, f0, [1.0, 2.7], [1.0, 0.08], [0.03, 0.012], detune=0.0)
    return x + 0.2 * strike(len(t), rng, 4000, 1.0, 0.0005, 0.003), midi(f0)


def fmbell(fc=523.25, ratio=1.4, i0=5.0, tau_i=0.12, tau_a=0.16, sec=1.0):
    t = tt(sec)
    return fm(t, fc, ratio, i0, tau_i, tau_a), midi(fc)


# (name, function, kwargs, maximum length in seconds)
BANK = [
    ("808",       s808,   {}, 0.45),
    ("808 SHORT", s808,   dict(fast=0.006, slow=0.035), 0.22),
    ("808 LO",    s808,   dict(f1=270, f2=400, bp=1320, fast=0.014, slow=0.1), 0.6),
    ("808 HI",    s808,   dict(f1=675, f2=1000, bp=3300, slow=0.06), 0.35),
    ("CR",        scr78,  {}, 0.22),
    ("CR HI",     scr78,  dict(f1=980, f2=1470, tau=0.02, seed=11), 0.17),
    ("CR LO",     scr78,  dict(f1=600, f2=900, tau=0.035, seed=19), 0.25),
    ("REAL",      real,   {}, 0.7),
    ("REAL MUT",  real,   dict(damp=0.28, hit=0.45, seed=12), 0.25),
    ("REAL HARD", real,   dict(bright=1.35, hit=0.8, strike_fc=5000, seed=13), 0.7),
    ("REAL LO",   real,   dict(f0=415, damp=1.3, seed=14), 0.66),
    ("REAL HI",   real,   dict(f0=740, damp=0.85, seed=15), 0.5),
    ("BIG",       big,    {}, 0.8),
    ("AGOGO HI",  agogo,  dict(f0=1180, tau=0.09, seed=16), 0.55),
    ("AGOGO LO",  agogo,  dict(f0=830, tau=0.11, seed=17), 0.6),
    ("TINY",      tiny,   {}, 0.3),
    ("CLANK",     clank,  {}, 0.3),
    ("RUSTY",     rusty,  {}, 0.5),
    ("TIN",       tin,    {}, 0.35),
    ("BLOCK",     block,  {}, 0.15),
    ("BLOCK HI",  block,  dict(f0=1650, seed=18), 0.12),
    ("CLAVE",     clave,  {}, 0.2),
    ("FM BELL",   fmbell, dict(i0=4.0, tau_a=0.11), 0.68),
    ("FM CLANG",  fmbell, dict(fc=330, ratio=3.53, i0=7.0, tau_i=0.05, tau_a=0.09), 0.55),
    ("FM SHORT",  fmbell, dict(fc=659.26, ratio=1.4, i0=3.5, tau_i=0.04, tau_a=0.06), 0.35),
    ("FM GLASS",  fmbell, dict(fc=987.77, ratio=2.76, i0=2.0, tau_i=0.08, tau_a=0.09), 0.5),
]


# ---------------------------------------------------------------------------------------------------------
# Conditioning: DC removal, trim, normalize

def trim(x, max_sec, cap_fade=0.03):
    """Start at the attack, end at -60 dB (or max_sec), fade the last few ms to exactly 0.

    A recording's noise floor can sit near -60 dB: the end threshold is then raised to 8 dB over the floor
    (measured, window by window like the envelope, on the silence before the attack), so hiss is not kept."""
    x = biquad(x, "hp", 30)                                       # no DC, no sub rumble
    a = np.abs(x)
    start = int(np.argmax(a > a.max() * 10 ** (-50 / 20)))
    win = int(0.005 * FS)
    thresh = 1e-3                                                 # -60 dB of the peak
    pre = a[:max(0, start - 4 * win)]                             # the lead-in, short of the attack
    if len(pre) >= 20 * win:
        npre = len(pre) // win
        floor = float(np.median(pre[:npre * win].reshape(npre, win).max(axis=1))) / a.max()
        thresh = max(thresh, floor * 10 ** (8 / 20))
    x = x[start:]
    # envelope: the peak over 5 ms windows; end where it stays below the threshold
    nwin = len(x) // win
    env = np.abs(x[:nwin * win]).reshape(nwin, win).max(axis=1)
    above = np.nonzero(env > env.max() * thresh)[0]
    end = (above[-1] + 1) * win if len(above) else len(x)
    capped = end > int(max_sec * FS)
    end = min(end, int(max_sec * FS), len(x))
    # level at the cut, dB below the peak (about -60 unless cut short at max_sec)
    cut_db = 20 * math.log10(max(env[min(end // win, nwin) - 1], 1e-12) / env.max())
    x = x[:end].copy()
    fade = int((cap_fade if capped else 0.005) * FS)             # a longer fade if we cut it short
    x[-fade:] *= 0.5 * (1 + np.cos(np.linspace(0, math.pi, fade)))
    x[-1] = 0.0
    return x / np.max(np.abs(x)) * PEAK, round(cut_db)


def spectral_peak(x, lo=60, hi=FS / 2):
    seg = x[:int(0.1 * FS)]
    seg = seg * np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(seg, 1 << 16))
    freqs = np.fft.rfftfreq(1 << 16, 1 / FS)
    spec[(freqs < lo) | (freqs > hi)] = 0
    return freqs[int(np.argmax(spec))]


def head_rms(x):
    return float(np.sqrt(np.mean(x[:int(0.1 * FS)] ** 2)))


# ---------------------------------------------------------------------------------------------------------
# WAV in / out

def read_wav(path):
    """Read a RIFF WAV: PCM 8/16/24/32-bit or float 32/64, any channels, any rate. Returns mono float, rate."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF WAVE file")
    pos, fmt, raw = 12, None, None
    while pos + 8 <= len(data):
        cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + size]
        if cid == b"fmt ":
            tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
            if tag == 0xFFFE:                                    # WAVE_FORMAT_EXTENSIBLE
                tag = struct.unpack("<H", body[24:26])[0]
            fmt = (tag, ch, rate, bits)
        elif cid == b"data":
            raw = body
        pos += 8 + size + (size & 1)
    if fmt is None or raw is None:
        raise ValueError("no fmt or data chunk")
    tag, ch, rate, bits = fmt
    width = bits // 8
    raw = raw[:len(raw) // (width * ch) * width * ch]
    if tag == 3:
        x = np.frombuffer(raw, dtype="<f4" if bits == 32 else "<f8").astype(np.float64)
    elif tag == 1:
        if bits == 8:
            x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128) / 128
        elif bits == 24:
            b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
            v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
            x = np.where(v >= 1 << 23, v - (1 << 24), v) / float(1 << 23)
        else:
            x = np.frombuffer(raw, dtype="<i2" if bits == 16 else "<i4").astype(np.float64) / 2.0 ** (bits - 1)
    else:
        raise ValueError(f"unsupported WAV format tag {tag}")
    return x.reshape(-1, ch).mean(axis=1), rate


def resample(x, rate):
    """Band-limited resampling to 44.1 kHz by FFT (fine for one-shots: pads first to avoid wrap-around)."""
    if rate == FS:
        return x
    n_out = int(round(len(x) * FS / rate))
    pad = len(x) + int(0.05 * rate)
    spec = np.fft.rfft(x, pad)
    m = int(round(pad * FS / rate))
    out = np.fft.irfft(spec, m) * (m / pad) if m // 2 + 1 <= len(spec) else \
        np.fft.irfft(np.concatenate([spec, np.zeros(m // 2 + 1 - len(spec))]), m) * (m / pad)
    return out[:n_out]


def half_rate(x):
    """44.1 kHz -> 22.05 kHz: a band-limited halving by FFT (padded first, like resample())."""
    pad = len(x) + int(0.05 * FS)
    pad += pad & 1
    spec = np.fft.rfft(x, pad)
    m = pad // 2
    spec = spec[:m // 2 + 1].copy()
    k = len(spec)                                  # a gentle roll-off over the top 10 %, not a brick wall
    t = int(k * 0.9)
    spec[t:] *= 0.5 * (1 + np.cos(np.linspace(0, math.pi, k - t)))
    y = np.fft.irfft(spec, m) * (m / pad)
    return y[:(len(x) + 1) // 2]


def write_wav(path, pcm, rate=FS):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(pcm.astype("<i2").tobytes())


# ---------------------------------------------------------------------------------------------------------

def c_name(name):
    return "cb_" + "".join(c.lower() if c.isalnum() else "_" for c in name)


BANK_OUT = os.path.join(ROOT, "out", "bank.bin")
BANK_MAX = 516096                                  # fw/: the user partition (536576) less the pattern store (20 KiB)


def write_bank(entries):
    """The bank as the FM-1 firmware reads it from flash (fw/app/cowbell_host.c), little-endian:
    header  'CBNK', u16 version 1, u16 count, u32 size (all of it), u32 CRC-32 of bytes 16..size
    entries count x 24: name[10] (NUL padded), root, gain, index0, fmt (0: IMA ADPCM), rshift (the rate:
            44.1 kHz >> rshift, 0 or 1), pad, u32 frames,
            u32 offset of the data from the bank's start
    data    each sample's bytes, 4-byte aligned"""
    import struct
    import zlib
    head = 16 + 24 * len(entries)
    off, table, blob = head, b"", b""
    for name, root, gain, index0, n, data, rshift in entries:
        nm = name.encode("ascii")[:9].ljust(10, b"\0")
        table += nm + struct.pack("<BBBBBxII", root, gain, index0, 0, rshift, n, off)
        pad = (-len(data)) % 4
        blob += bytes(data) + b"\0" * pad
        off += len(data) + pad
    body = table + blob
    size = 16 + len(body)
    if size > BANK_MAX:
        sys.exit(f"FAIL: the bank is {size} bytes, over the FM-1's {BANK_MAX}")
    bank = b"CBNK" + struct.pack("<HHII", 1, len(entries), size, zlib.crc32(body) & 0xFFFFFFFF) + body
    os.makedirs(os.path.dirname(BANK_OUT), exist_ok=True)
    with open(BANK_OUT, "wb") as f:
        f.write(bank)
    return bank


STARTER_C = os.path.join(ROOT, "fw", "app", "cowbell", "starter.c")


def write_c(path, entries, table, count, what):
    used, order = set(), []                        # unique C identifiers, in bank order
    for name, *_ in entries:
        cn = c_name(name)
        while cn in used:
            cn += "_"
        used.add(cn)
        order.append(cn)
    lines = [f"/* {what}: generated by tools/make_samples.py. Do not edit by hand. */",
             '#include "samples.h"', ""]
    for (name, root, gain, index0, n, data, rshift), cn in zip(entries, order):
        lines.append(f"/* {name}: root {root}, {n} frames at {FS >> rshift} Hz */")
        lines.append(f"static const uint8_t {cn}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
        lines.append("")
    lines.append(f"const cb_sample_t {table}[] = {{")
    for (name, root, gain, index0, n, data, rshift), cn in zip(entries, order):
        lines.append(f'    {{ "{name}", {root}, {gain}, {index0}, {rshift}, {n}u, {cn} }},')
    lines.append("};")
    lines.append(f"const uint32_t {count} = {len(entries)};")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--wav", metavar="DIR", help="also import every .wav in DIR, appended after the built-ins")
    ap.add_argument("--no-builtin", action="store_true", help="leave out the synthesized bank")
    ap.add_argument("--keep", metavar="NAMES", help="only these synthesized sounds, comma-separated (e.g. '808,CR')")
    ap.add_argument("--max-len", type=float, default=1.0, help="longest imported sample, seconds (1.0)")
    ap.add_argument("--starter", action="store_true",
                    help="the FM-1 firmware's compiled-in starter bank instead: 22.05 kHz, written only to "
                         "fw/app/cowbell/starter.c (previews in out/samples_starter); give --max-len to fit")
    ap.add_argument("--starter-max", type=int, default=47 * 1024, help="--starter: most bytes of sample data")
    ap.add_argument("--min-snr", type=float, default=MIN_SNR, help=f"fail below this SNR, dB ({MIN_SNR})")
    ap.add_argument("--cap-fade", type=float, default=0.03, help="fade at a --max-len cut, seconds (0.03)")
    ap.add_argument("--save-trimmed", metavar="DIR",
                    help="also write each imported WAV, trimmed, as 16-bit 44.1 kHz mono (before ADPCM), to DIR")
    args = ap.parse_args()

    # 1. make (or import) and condition every sound
    items = []                                     # (name, audio float, root, capped)
    if not args.no_builtin:
        keep = {k.strip().upper() for k in args.keep.split(",")} if args.keep else None
        for name, fn, kw, max_sec in BANK:
            if keep is not None and name not in keep:
                continue
            x, root = fn(**kw)
            x, capped = trim(x, max_sec, args.cap_fade)
            items.append((name, x, root, capped))
    if args.wav:
        for fname in sorted(os.listdir(args.wav)):
            if not fname.lower().endswith(".wav"):
                continue
            try:
                x, rate = read_wav(os.path.join(args.wav, fname))
            except ValueError as e:
                print(f"skipping {fname}: {e}", file=sys.stderr)
                continue
            x, capped = trim(resample(x, rate), args.max_len, args.cap_fade)
            stem = re.sub(r"^\d+[ _-]+", "", os.path.splitext(fname)[0])   # "01 STICK 1" -> "STICK 1" (the number orders)
            name = "".join(c for c in stem.upper().replace("_", " ")
                           if 32 <= ord(c) < 127 and c not in '"\\')[:9].strip() or "WAV"
            items.append((name, x, midi(spectral_peak(x, 250, 4000)), capped))   # a bell's pitch: not a thud, not a click
            if args.save_trimmed:
                os.makedirs(args.save_trimmed, exist_ok=True)
                write_wav(os.path.join(args.save_trimmed, fname), np.round(x * 32767))
    if not items:
        sys.exit("no samples")

    # 2. loudness: match the RMS of the first 100 ms; quieter-than-target samples stay at 1.0
    rms = [head_rms(x) for _, x, _, _ in items]
    target = float(np.percentile(rms, 25))
    gains = [max(16, min(128, int(round(128 * target / r)))) for r in rms]

    # 3. encode, verify with the reference decoder, write previews
    rshift = 1 if args.starter else 0
    rate = FS >> rshift
    out_wav = OUT_WAV + "_starter" if args.starter else OUT_WAV
    os.makedirs(out_wav, exist_ok=True)
    for f in os.listdir(out_wav):
        if f.endswith(".wav"):
            os.remove(os.path.join(out_wav, f))
    entries, chain, total, worst = [], [], 0, 99.0
    print(f"{'#':>2} {'name':<9} {'root':>4} {'note':<4} {'peakHz':>7} {'ms':>5} {'bytes':>6} "
          f"{'SNR dB':>6} {'idx0':>4} {'gain':>4} {'DC':>8} {'end dB':>6}")
    for k, ((name, x, root, capped), gain) in enumerate(zip(items, gains)):
        peak_hz = spectral_peak(x)
        if rshift:
            x = half_rate(x)
            x = x / np.max(np.abs(x)) * PEAK
        pcm = np.round(x * 32767).astype(np.int64)
        index0, data = encode_best(pcm.tolist())
        dec = adpcm_decode(data, len(pcm), index0)
        snr = snr_db(pcm, dec)
        worst = min(worst, snr)
        total += len(data)
        notes = "C C# D D# E F F# G G# A A# B".split()
        note = f"{notes[root % 12]}{root // 12 - 1}"
        print(f"{k + 1:>2} {name:<9} {root:>4} {note:<4} {peak_hz:>7.0f} {1000 * len(pcm) / rate:>5.0f} "
              f"{len(data):>6} {snr:>6.1f} {index0:>4} {gain:>4} {np.mean(x):>8.1e}"
              f" {capped:>6}")
        entries.append((name, root, gain, index0, len(pcm), data, rshift))
        play = np.clip(dec.astype(np.int64) * gain // 128, -32768, 32767)
        write_wav(os.path.join(out_wav, f"{k + 1:02d}_{name.replace(' ', '_')}.wav"), play, rate)
        chain += [play, np.zeros(int(0.2 * rate), dtype=np.int64)]
    write_wav(os.path.join(out_wav, "all.wav"), np.concatenate(chain), rate)
    print(f"total {total} bytes = {total / 1024:.1f} KiB in {len(entries)} samples "
          f"({sum(e[4] for e in entries) / rate:.2f} s at {rate} Hz); budget {BUDGET // 1024} KiB, hard max {HARD_MAX // 1024} KiB")
    print(f"worst SNR {worst:.1f} dB")

    if worst < args.min_snr:
        sys.exit(f"FAIL: an SNR is below {args.min_snr} dB")

    if args.starter:
        if total > args.starter_max:
            sys.exit(f"FAIL: {total} bytes is over the starter's {args.starter_max}: give a smaller --max-len")
        write_c(STARTER_C, entries, "cb_starter", "cb_starter_n",
                "The FM-1 firmware's starter bank (22.05 kHz), compiled in and played until a bank is uploaded")
        print(f"wrote {STARTER_C} and {out_wav}")
        return

    # 4a. the bank for the FM-1 firmware (fw/): uploaded into its flash by tools/cowbell_upload.py
    bank = write_bank(entries)
    print(f"wrote {BANK_OUT} ({len(bank)} bytes of {BANK_MAX})")
    if total > HARD_MAX:
        sys.exit(f"{total} bytes is over fm1-dock's maximum of {HARD_MAX}; samples.c not written (bank.bin is)")

    # 4. the C file
    write_c(OUT_C, entries, "CB_SAMPLES", "CB_N_SAMPLES", "The cowbell sample bank")
    print(f"wrote {OUT_C} and {OUT_WAV}")

if __name__ == "__main__":
    main()
