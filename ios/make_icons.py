#!/usr/bin/env python3
"""Generate opaque, centered iOS app icons from assets/Icons/MISRC_Icon.png.

Why this exists (2026-11-08): the source icon is a 500x500 RGBA artwork of a
rounded square WITH a soft drop shadow below it and transparent corners.
iOS home screens render transparency as black voids (and App Store
validation rejects alpha in icons), and the shadow makes a straight resize
look bottom-heavy/off-center. iOS applies its OWN rounded-corner mask, so
the correct treatment is:

  1. locate the square artwork (the bright frame border), excluding the
     drop shadow,
  2. crop to that square, keeping a tiny margin so the frame is not cut,
  3. flatten over the artwork's own interior background color (so the
     rounded corners of the art blend into an opaque square),
  4. downscale (box filter) to each target size with alpha forced to 255.

Pure Python (no PIL): ships its own PNG decode/encode. Output sizes are the
four icons referenced by ios/Info.plist (CFBundleIconFiles):
  AppIcon60x60@2x.png (120), AppIcon60x60@3x.png (180),
  AppIcon76x76@2x~ipad.png (152), AppIcon83.5x83.5@2x~ipad.png (167).

Usage: make_icons.py <source.png> <output_dir> [--bg auto|R,G,B] [--margin N]
"""
import os
import sys
import struct
import zlib


# ---------------------------------------------------------------- PNG decode
def decode_png(path):
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('not a PNG: %s' % path)
    pos = 8
    idat = b''
    w = h = bd = ct = 0
    palette = b''
    trns = b''
    while pos < len(data):
        ln, typ = struct.unpack('>I4s', data[pos:pos + 8])
        pos += 8
        chunk = data[pos:pos + ln]
        pos += ln + 4
        if typ == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', chunk[:10])
        elif typ == b'IDAT':
            idat += chunk
        elif typ == b'PLTE':
            palette = chunk
        elif typ == b'tRNS':
            trns = chunk
        elif typ == b'IEND':
            break
    if bd != 8:
        raise SystemExit('unsupported bit depth %d' % bd)
    raw = zlib.decompress(idat)
    if ct == 6:
        bpp = 4
    elif ct == 2:
        bpp = 3
    elif ct == 3:
        bpp = 1
    else:
        raise SystemExit('unsupported color type %d' % ct)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        f = raw[i]
        i += 1
        line = bytearray(raw[i:i + stride])
        i += stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out += line
        prev = line
    # normalize to RGBA bytearray
    rgba = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            o = y * stride + x * bpp
            d = (y * w + x) * 4
            if ct == 6:
                rgba[d:d + 4] = out[o:o + 4]
            elif ct == 2:
                rgba[d:d + 3] = out[o:o + 3]
                rgba[d + 3] = 255
            elif ct == 3:
                idx = out[o]
                rgba[d:d + 3] = palette[idx * 3:idx * 3 + 3]
                rgba[d + 3] = trns[idx] if idx < len(trns) else 255
    return w, h, rgba


# ---------------------------------------------------------------- PNG encode
def encode_png_rgb(path, w, h, rgb):
    def chunk(typ, payload):
        return (struct.pack('>I', len(payload)) + typ + payload +
                struct.pack('>I', zlib.crc32(typ + payload) & 0xffffffff))
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)  # filter: none
        raw += rgb[y * stride:(y + 1) * stride]
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)
    data = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
    open(path, 'wb').write(data)


# ---------------------------------------------------------------- resampling
def box_downscale_rgba(src, sw, sh, dw, dh):
    """Area-average downscale of an RGBA buffer; alpha forced opaque output
    is handled by the caller (we composite before scaling)."""
    dst = bytearray(dw * dh * 4)
    for y in range(dh):
        y0 = y * sh // dh
        y1 = max(y0 + 1, (y + 1) * sh // dh)
        for x in range(dw):
            x0 = x * sw // dw
            x1 = max(x0 + 1, (x + 1) * sw // dw)
            r = g = b = a = 0
            n = 0
            for yy in range(y0, y1):
                rowbase = yy * sw
                for xx in range(x0, x1):
                    o = (rowbase + xx) * 4
                    ar = src[o + 3]
                    r += src[o] * ar
                    g += src[o + 1] * ar
                    b += src[o + 2] * ar
                    a += ar
                    n += 1
            d = (y * dw + x) * 4
            if a > 0:
                dst[d] = min(255, r // a)
                dst[d + 1] = min(255, g // a)
                dst[d + 2] = min(255, b // a)
                dst[d + 3] = min(255, a // n)
            else:
                dst[d:d + 4] = (0, 0, 0, 255)
    return dst


def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else 'assets/Icons/MISRC_Icon.png'
    out_dir = sys.argv[2] if len(sys.argv) > 2 else '.'
    bg = 'auto'
    margin = 6
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        if args[i] == '--bg':
            bg = args[i + 1]
            i += 2
        elif args[i] == '--margin':
            margin = int(args[i + 1])
            i += 2
        else:
            raise SystemExit('unknown arg %r' % args[i])

    w, h, rgba = decode_png(src_path)
    print('source: %dx%d' % (w, h))
    os.makedirs(out_dir, exist_ok=True)

    # --- locate the square artwork via full-width alpha coverage ---
    # The icon art is a rounded square spanning nearly the full canvas
    # width; the desktop-style base/shine elements BELOW it are narrower.
    # Rows of the square have ~full opaque coverage; rows below do not.
    # (A luminance-based frame detection fails here: the interior contains
    # bright scope-trace art of its own.)
    rowcount = [0] * h
    for yy in range(h):
        c = 0
        for xx in range(w):
            if rgba[(yy * w + xx) * 4 + 3] > 200:
                c += 1
        rowcount[yy] = c
    peak = max(rowcount)
    wide = 0.8 * peak
    # longest contiguous run of wide rows = the square's vertical extent
    best_len = 0
    best_top = 0
    run_start = None
    for yy in range(h):
        if rowcount[yy] >= wide:
            if run_start is None:
                run_start = yy
        else:
            if run_start is not None:
                if yy - run_start > best_len:
                    best_len = yy - run_start
                    best_top = run_start
                run_start = None
    if run_start is not None and h - run_start > best_len:
        best_len = h - run_start
        best_top = run_start
    if best_len < h // 4:
        raise SystemExit('no full-width square region found in icon')
    sq_top = best_top
    sq_bottom = best_top + best_len - 1
    # horizontal extent: columns well-covered within the square rows
    colcount = [0] * w
    for xx in range(w):
        c = 0
        for yy in range(sq_top, sq_bottom + 1):
            if rgba[(yy * w + xx) * 4 + 3] > 200:
                c += 1
        colcount[xx] = c
    sq_height = sq_bottom - sq_top + 1
    cols_wide = [xx for xx in range(w) if colcount[xx] >= 0.8 * sq_height]
    if not cols_wide:
        raise SystemExit('no full-height column range found in icon')
    sq_left = cols_wide[0]
    sq_right = cols_wide[-1]
    print('square (alpha coverage): x %d..%d  y %d..%d' %
          (sq_left, sq_right, sq_top, sq_bottom))

    # --- art region: the detected square artwork, flattened over an opaque
    # background (the art's rounded corners blend into its own interior
    # color; the below-square base/shine elements are EXCLUDED). ---
    aw = sq_right - sq_left + 1
    ah = sq_bottom - sq_top + 1
    print('art region: %dx%d' % (aw, ah))

    # background color: average of the opaque interior of the art region
    if bg == 'auto':
        rs = gs = bs = n = 0
        for yy in range(sq_top + ah // 6, sq_top + 5 * ah // 6, 3):
            for xx in range(sq_left + aw // 6, sq_left + 5 * aw // 6, 3):
                o = (yy * w + xx) * 4
                if rgba[o + 3] > 200:
                    rs += rgba[o]
                    gs += rgba[o + 1]
                    bs += rgba[o + 2]
                    n += 1
        bgc = (rs // n, gs // n, bs // n) if n else (0, 0, 0)
    else:
        bgc = tuple(int(v) for v in bg.split(','))
    print('background: %s' % (bgc,))

    artflat = bytearray(aw * ah * 4)
    for yy in range(ah):
        for xx in range(aw):
            so = ((sq_top + yy) * w + (sq_left + xx)) * 4
            a = rgba[so + 3]
            d = (yy * aw + xx) * 4
            if a == 255:
                artflat[d:d + 4] = rgba[so:so + 4]
            elif a == 0:
                artflat[d:d + 3] = bgc
                artflat[d + 3] = 255
            else:
                ia = 255 - a
                artflat[d] = (rgba[so] * a + bgc[0] * ia + 127) // 255
                artflat[d + 1] = (rgba[so + 1] * a + bgc[1] * ia + 127) // 255
                artflat[d + 2] = (rgba[so + 2] * a + bgc[2] * ia + 127) // 255
                artflat[d + 3] = 255

    # --- emit the four sizes: opaque square, art scale-to-FIT centered ---
    # (the artwork is a wide rounded-rect, so a square crop would cut the
    # frame; fit the WHOLE art with a uniform margin of background color)
    sizes = [
        ('AppIcon60x60@2x.png', 120),
        ('AppIcon60x60@3x.png', 180),
        ('AppIcon76x76@2x~ipad.png', 152),
        ('AppIcon83.5x83.5@2x~ipad.png', 167),
    ]
    padfrac = margin / 100.0  # margin px of source scale -> fraction knob
    inner = 1.0 - 2 * (padfrac if padfrac > 0 else 0.07)
    for name, size in sizes:
        w2 = max(1, round(size * inner * aw / max(aw, ah)))
        h2 = max(1, round(size * inner * ah / max(aw, ah)))
        scaled = box_downscale_rgba(artflat, aw, ah, w2, h2)
        ox = (size - w2) // 2
        oy = (size - h2) // 2
        rgb = bytearray(size * size * 3)
        # background fill
        for p in range(size * size):
            d = p * 3
            rgb[d:d + 3] = bgc
        # paste the scaled art (already opaque)
        for yy in range(h2):
            for xx in range(w2):
                so = (yy * w2 + xx) * 4
                d = ((oy + yy) * size + (ox + xx)) * 3
                rgb[d] = scaled[so]
                rgb[d + 1] = scaled[so + 1]
                rgb[d + 2] = scaled[so + 2]
        encode_png_rgb('%s/%s' % (out_dir, name), size, size, rgb)
        print('wrote %s/%s (%dx%d, opaque, art %dx%d centered)' %
              (out_dir, name, size, size, w2, h2))


if __name__ == '__main__':
    main()
