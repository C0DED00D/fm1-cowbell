#!/usr/bin/env python3
"""Render the COWBELL knob pictures and write cowbell/art.c (the format of cowbell/art.h).

Each picture is drawn at SS x resolution in float RGB, box-downsampled (antialiasing), then quantized
to 16 colours: index 0 is the background it sits on, the other 15 come from a small k-means over the
knob's own pixels. Pixels are packed 4 bits each, high nibble = left pixel, rows byte-aligned.

Previews (decoded back from the packed data, i.e. what the device shows) go to out/art/.

    python tools/make_art.py
"""
import math
import os

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ART_C = os.path.join(ROOT, "cowbell", "art.c")
OUT = os.path.join(ROOT, "out", "art")
SS = 4  # supersampling factor

COL_PANEL = (237, 147, 177)  # CB_COL_PANEL, behind the small knobs
COL_MORE = (212, 83, 126)    # CB_COL_MORE, the square behind the MORE knob


def rgb565(c):
    r, g, b = (int(round(v)) for v in c)
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def rgb565_to_rgb(v):
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 3))


# ---------------------------------------------------------------- drawing helpers (supersampled space)

class Canvas:
    """A float RGB canvas at SS x, centre (cx, cy) given in output pixels (pixel centres)."""

    def __init__(self, w, h, cx, cy, bg):
        self.W, self.H = w * SS, h * SS
        self.rgb = np.empty((self.H, self.W, 3), np.float64)
        self.rgb[:] = bg
        self.knob = np.zeros((self.H, self.W))  # coverage of anything that is not background
        ys, xs = np.mgrid[0:self.H, 0:self.W]
        # coordinates in output-pixel units, relative to the knob centre
        self.x = (xs + 0.5) / SS - (cx + 0.5)
        self.y = (ys + 0.5) / SS - (cy + 0.5)
        self.r = np.hypot(self.x, self.y)

    def paint(self, mask, colour):
        """Composite colour (array HxWx3 or a tuple) where mask (0..1) says."""
        m = np.clip(mask, 0, 1)[..., None]
        self.rgb = self.rgb * (1 - m) + np.asarray(colour, np.float64) * m
        self.knob = np.maximum(self.knob, m[..., 0])

    def disc(self, radius):
        # a hard-edged disc at SS x; the box downsample makes the edge soft
        return (self.r <= radius).astype(np.float64)

    def tick(self, deg, r_in, r_out, width):
        """A straight line along angle deg (0 = up, clockwise), from r_in to r_out, rounded ends off."""
        a = math.radians(deg)
        ux, uy = math.sin(a), -math.cos(a)
        along = self.x * ux + self.y * uy
        across = np.abs(-self.x * uy + self.y * ux)
        return ((along >= r_in) & (along <= r_out) & (across <= width / 2)).astype(np.float64)

    def result(self):
        """Downsample to output size: (rgb float HxWx3, knob coverage HxW)."""
        h, w = self.H // SS, self.W // SS
        rgb = self.rgb.reshape(h, SS, w, SS, 3).mean(axis=(1, 3))
        cov = self.knob.reshape(h, SS, w, SS).mean(axis=(1, 3))
        return rgb, cov


def mix(a, b, t):
    """Blend colours a -> b by t (t may be an array)."""
    t = np.clip(np.asarray(t, np.float64), 0, 1)[..., None]
    return np.asarray(a, np.float64) * (1 - t) + np.asarray(b, np.float64) * t


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


# ---------------------------------------------------------------- the two knobs

def draw_more():
    w = h = 112
    cx = cy = 56
    cap_r, skirt_r = 39.0, 44.0
    c = Canvas(w, h, cx, cy, COL_MORE)

    # 7 tick marks, -135 .. +135 degrees, outside the skirt
    for deg in range(-135, 136, 45):
        c.paint(c.tick(deg, 47.0, 54.0, 3.0), (74, 18, 46))

    # skirt: a darker pinkish-grey ring, lit from the top left, darker toward the bottom right
    light = (-c.x - c.y) / (skirt_r * 1.414)       # +1 top-left .. -1 bottom-right
    skirt = mix((140, 72, 102), (224, 178, 198), smooth(light * 0.5 + 0.5))
    c.paint(c.disc(skirt_r), skirt)
    # a thin dark groove where the cap sits in the skirt
    c.paint(c.disc(cap_r + 1.2), (128, 70, 96))

    # cap: pale pink plastic, a soft radial highlight up and to the left, darker toward the edge
    hx, hy = -12.0, -14.0
    d = np.hypot(c.x - hx, c.y - hy) / (cap_r * 1.6)
    base = mix((255, 246, 250), (226, 172, 194), smooth(d))
    # a gentle darkening right at the cap's rim so the edge reads
    edge = smooth((c.r - (cap_r - 4.0)) / 4.0)
    base = mix(base, (205, 150, 174), edge * 0.6)
    c.paint(c.disc(cap_r), base)
    return c, dict(w=w, h=h, cx=cx, cy=cy, r0=10, r1=int(cap_r) - 5, pointer=(92, 22, 62))


def draw_small():
    w = h = 44
    cx = cy = 22
    knob_r, face_r = 18.0, 13.5
    c = Canvas(w, h, cx, cy, COL_PANEL)

    # body: dark magenta, lit from the top left
    light = (-c.x - c.y) / (knob_r * 1.414)
    body = mix((112, 20, 66), (196, 62, 128), smooth(light * 0.5 + 0.5))
    c.paint(c.disc(knob_r), body)
    # a dark line where the top face meets the body
    c.paint(c.disc(face_r + 1.0), (96, 16, 56))
    # top face: lighter magenta with a soft highlight
    d = np.hypot(c.x + 4.0, c.y + 5.0) / (face_r * 1.7)
    face = mix((236, 112, 170), (180, 46, 112), smooth(d))
    c.paint(c.disc(face_r), face)
    return c, dict(w=w, h=h, cx=cx, cy=cy, r0=4, r1=int(face_r) - 2, pointer=(255, 236, 244))


# ---------------------------------------------------------------- quantize and pack

def quantize(rgb, cov, bg, n=16, iters=40):
    """Return (palette list of 16 RGB565 ints, index array). Index 0 = bg, forced where cov is ~0."""
    bg565 = rgb565(bg)
    pts = rgb[cov > 0.02].reshape(-1, 3)
    wts = cov[cov > 0.02].reshape(-1)
    # initial centres: spread along luminance
    lum = pts @ np.array([0.299, 0.587, 0.114])
    order = np.argsort(lum)
    k = n - 1
    centres = pts[order[np.linspace(0, len(order) - 1, k).astype(int)]].copy()
    allc = None
    for _ in range(iters):
        allc = np.vstack([np.asarray(bg, np.float64), centres])
        dist = ((pts[:, None, :] - allc[None]) ** 2).sum(-1)
        lab = dist.argmin(1)
        for j in range(k):
            sel = lab == j + 1
            if sel.any():
                ww = wts[sel][:, None]
                centres[j] = (pts[sel] * ww).sum(0) / ww.sum()
    # palette as RGB565, then map every pixel to the nearest decoded palette colour
    pal = [bg565] + [rgb565(cc) for cc in centres]
    dec = np.array([rgb565_to_rgb(v) for v in pal], np.float64)
    dist = ((rgb[..., None, :] - dec[None, None]) ** 2).sum(-1)
    idx = dist.argmin(-1).astype(np.uint8)
    idx[cov <= 0.02] = 0
    return pal, idx


def pack(idx):
    h, w = idx.shape
    out = bytearray()
    for y in range(h):
        row = list(idx[y]) + ([0] if w % 2 else [])
        for x in range(0, len(row), 2):
            out.append((row[x] << 4) | row[x + 1])
    return bytes(out)


def unpack(data, w, h, pal):
    """Decode the packed data back to an RGB image, exactly as the device will see it."""
    stride = (w + 1) // 2
    img = Image.new("RGB", (w, h))
    pix = img.load()
    for y in range(h):
        for x in range(w):
            b = data[y * stride + x // 2]
            i = (b >> 4) if x % 2 == 0 else (b & 15)
            pix[x, y] = rgb565_to_rgb(pal[i])
    return img


# ---------------------------------------------------------------- output

def c_array(ctype, name, values, per_line, fmt):
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(fmt(v) for v in values[i:i + per_line]) + ",")
    return f"static const {ctype} {name}[{len(values)}] = {{\n" + "\n".join(lines) + "\n};\n"


def preview(name, img, meta, pal_bg):
    img.save(os.path.join(OUT, f"{name}_1x.png"))
    img.resize((img.width * 4, img.height * 4), Image.NEAREST).save(os.path.join(OUT, f"{name}_4x.png"))
    # mock: the pointer at a few angles, as the C code would draw it (thick line + a dot), 4x zoom
    frames = []
    for deg in (-135, 0, 90):
        f = img.copy().resize((img.width * SS, img.height * SS), Image.NEAREST)
        d = ImageDraw.Draw(f)
        a = math.radians(deg)
        cx, cy = (meta["cx"] + 0.5) * SS, (meta["cy"] + 0.5) * SS
        p0 = (cx + math.sin(a) * meta["r0"] * SS, cy - math.cos(a) * meta["r0"] * SS)
        p1 = (cx + math.sin(a) * meta["r1"] * SS, cy - math.cos(a) * meta["r1"] * SS)
        col = rgb565_to_rgb(rgb565(meta["pointer"]))
        lw = 3 * SS if meta["w"] > 60 else 2 * SS
        d.line([p0, p1], fill=col, width=lw)
        for p in (p0, p1):
            d.ellipse([p[0] - lw / 2, p[1] - lw / 2, p[0] + lw / 2, p[1] + lw / 2], fill=col)
        frames.append(f.resize((img.width, img.height), Image.LANCZOS))
    gap = 8
    mock = Image.new("RGB", (len(frames) * (img.width + gap) + gap, img.height + 2 * gap), rgb565_to_rgb(pal_bg))
    for i, f in enumerate(frames):
        mock.paste(f, (gap + i * (img.width + gap), gap))
    mock.save(os.path.join(OUT, f"{name}_mock_1x.png"))
    mock.resize((mock.width * 4, mock.height * 4), Image.NEAREST).save(os.path.join(OUT, f"{name}_mock_4x.png"))


def main():
    os.makedirs(OUT, exist_ok=True)
    parts = ['/* Generated by tools/make_art.py: do not edit by hand. */\n#include "art.h"\n']
    for cname, key, (canvas, meta), bg in (
            ("CB_ART_MORE", "more", draw_more(), COL_MORE),
            ("CB_ART_SMALL", "small", draw_small(), COL_PANEL)):
        rgb, cov = canvas.result()
        pal, idx = quantize(rgb, cov, bg)
        data = pack(idx)
        assert len(data) == (meta["w"] + 1) // 2 * meta["h"]
        assert idx[0, :].max() == 0 and idx[-1, :].max() == 0 and idx[:, 0].max() == 0 and idx[:, -1].max() == 0
        parts.append(c_array("uint16_t", f"{key}_pal", pal, 8, lambda v: f"0x{v:04X}"))
        parts.append(c_array("uint8_t", f"{key}_px", list(data), 16, lambda v: f"0x{v:02X}"))
        parts.append(
            f"const cb_img_t {cname} = {{ {meta['w']}, {meta['h']}, {meta['cx']}, {meta['cy']}, "
            f"{meta['r0']}, {meta['r1']}, 0x{rgb565(meta['pointer']):04X}, 0, {key}_pal, {key}_px }};\n")
        preview(key, unpack(data, meta["w"], meta["h"], pal), meta, pal[0])
        print(f"{cname}: {meta['w']}x{meta['h']}, {len(data)} bytes, cx={meta['cx']} cy={meta['cy']} "
              f"r0={meta['r0']} r1={meta['r1']} pointer=0x{rgb565(meta['pointer']):04X}")
    with open(ART_C, "w", newline="\n") as f:
        f.write("\n".join(parts))
    print("wrote", ART_C)


if __name__ == "__main__":
    main()
