#!/usr/bin/env python3
"""Generate opaque, centered iOS app icons from assets/Icons/MISRC_Icon.png.

Why this exists (2026-11-08): the source icon is a 500x500 RGBA artwork of a
rounded square WITH a soft drop shadow below it and transparent corners.
iOS home screens render transparency as black voids (and App Store
validation rejects alpha in icons), and the shadow makes a straight resize
look bottom-heavy/off-center. iOS applies its OWN rounded-corner mask, so
the correct treatment is:

  1. keep the COMPLETE original composition (window + stand + shadow),
  2. composite its alpha out over an opaque background (default: white,
     the surface the art is designed for),
  3. downscale (box filter) to each target size — no alpha channel.

Pure Python (no PIL): ships its own PNG decode/encode. Output sizes are the
four icons referenced by ios/Info.plist (CFBundleIconFiles):
  AppIcon60x60@2x.png (120), AppIcon60x60@3x.png (180),
  AppIcon76x76@2x~ipad.png (152), AppIcon83.5x83.5@2x~ipad.png (167).

Usage: make_icons.py <source.png> <output_dir> [--bg auto|R,G,B] [--margin N]

Default (--bg auto): faithful mode — the COMPLETE original artwork
(window + stand + soft shadow, already centered on its square canvas) is
flattened over WHITE, matching how the art is presented in the project
README. iOS renders transparent icon pixels as black, so the alpha is
composited out; no cropping, no letterboxing. The earlier crop-to-square
attempt (window only, interior-color bands) read as "mangled" — retired.
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

    # Background: default WHITE (the artwork — window, stand and soft
    # shadow — is designed for a light surface; that is how the README
    # presents it). --bg R,G,B overrides.
    if bg == 'auto':
        bgc = (255, 255, 255)
    else:
        bgc = tuple(int(v) for v in bg.split(','))
    print('background: %s' % (bgc,))

    # Faithful mode: flatten the COMPLETE source composition over the
    # background (alpha composited out -> opaque icon; iOS renders
    # transparent icon pixels as black, which is what broke the corners).
    # No crop, no letterbox: the art is already composed on its square
    # canvas and centered.
    artflat = bytearray(w * h * 4)
    for yy in range(h):
        for xx in range(w):
            so = (yy * w + xx) * 4
            a = rgba[so + 3]
            d = (yy * w + xx) * 4
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

    sizes = [
        ('AppIcon60x60@2x.png', 120),
        ('AppIcon60x60@3x.png', 180),
        ('AppIcon76x76@2x~ipad.png', 152),
        ('AppIcon83.5x83.5@2x~ipad.png', 167),
    ]
    for name, size in sizes:
        scaled = box_downscale_rgba(artflat, w, h, size, size)
        rgb = bytearray(size * size * 3)
        for p in range(size * size):
            o = p * 4
            d = p * 3
            if scaled[o + 3] == 255:
                rgb[d:d + 3] = scaled[o:o + 3]
            else:
                a = scaled[o + 3]
                ia = 255 - a
                rgb[d] = (scaled[o] * a + bgc[0] * ia + 127) // 255
                rgb[d + 1] = (scaled[o + 1] * a + bgc[1] * ia + 127) // 255
                rgb[d + 2] = (scaled[o + 2] * a + bgc[2] * ia + 127) // 255
        encode_png_rgb('%s/%s' % (out_dir, name), size, size, rgb)
        print('wrote %s/%s (%dx%d, opaque, full original art on white)' %
              (out_dir, name, size, size))


if __name__ == '__main__':
    main()
