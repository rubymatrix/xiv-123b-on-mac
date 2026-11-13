"""The Android app's icon, from the player's game files at build time (as the iPhone's, tools/ios_icon.m):
ffxivboot.exe's 256-pixel icon (else ffxivgame.exe's), its illustration without the logo, on white. It
becomes an adaptive icon (Android 8 and later, and the app needs 10): a white background layer and the
illustration as the foreground, inside the part every launcher's mask keeps. Python's standard library
only (zlib), so it runs wherever the build does.

  python3 tools/android_icon.py <ffxivboot.exe or ffxivgame.exe> <res folder>

Writes <res>/mipmap-anydpi-v26/ic_launcher.xml, <res>/drawable-nodpi/ic_launcher_art.png and
<res>/values/ic_launcher.xml; exits 1 when the exe has no icon it can read.
"""
import os
import struct
import sys
import zlib

# the illustration in the 256-pixel tile, clear of its frame and the logo (ios_icon.m's ART_*)
ART_X, ART_Y, ART_SIDE = 46 / 256, 14 / 256, 164 / 256
SIZE = 432       # the foreground layer at xxxhdpi: 108 dp x 4
ART_SCALE = 0.7  # the illustration's side as a share of the layer: within the 72 dp masks keep, nearly


def icon_png(exe):
    """the largest PNG among the exe's icons, as bytes, or None"""
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_RESOURCE']])
    best = None
    for t in pe.DIRECTORY_ENTRY_RESOURCE.entries if hasattr(pe, 'DIRECTORY_ENTRY_RESOURCE') else []:
        if t.id != 3:  # RT_ICON
            continue
        for e in t.directory.entries:
            for lang in e.directory.entries:
                d = pe.get_data(lang.data.struct.OffsetToData, lang.data.struct.Size)
                if d[:8] == b'\x89PNG\r\n\x1a\n' and (best is None or len(d) > len(best)):
                    best = d
    return best


def png_decode(data):
    """(width, height, RGBA rows as bytearrays) of an 8-bit RGB or RGBA PNG"""
    pos, idat, w = 8, b'', 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            if depth != 8 or ctype not in (2, 6) or interlace:
                raise ValueError('not an 8-bit RGB or RGBA PNG')
        elif kind == b'IDAT':
            idat += body
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev, at = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if bpp == 3:
            line = bytearray(b''.join(bytes(line[i:i + 3]) + b'\xff' for i in range(0, stride, 3)))
        rows.append(line)
    return w, h, rows


def png_encode(w, h, rows):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def sample(rows, w, h, x, y):
    """bilinear, RGBA, x and y in source pixels"""
    x0, y0 = max(0, min(w - 1, int(x))), max(0, min(h - 1, int(y)))
    x1, y1 = min(w - 1, x0 + 1), min(h - 1, y0 + 1)
    fx, fy = max(0.0, min(1.0, x - x0)), max(0.0, min(1.0, y - y0))
    out = []
    for k in range(4):
        top = rows[y0][4 * x0 + k] * (1 - fx) + rows[y0][4 * x1 + k] * fx
        bot = rows[y1][4 * x0 + k] * (1 - fx) + rows[y1][4 * x1 + k] * fx
        out.append(int(top * (1 - fy) + bot * fy + 0.5))
    return out


def foreground(w, h, rows):
    """the illustration, square, centred at ART_SCALE of a transparent SIZE x SIZE layer"""
    sx, sy, side = ART_X * w, ART_Y * h, ART_SIDE * w
    art = int(SIZE * ART_SCALE)
    off = (SIZE - art) // 2
    out = [bytearray(SIZE * 4) for _ in range(SIZE)]
    for y in range(art):
        for x in range(art):
            px = sample(rows, w, h, sx + (x + 0.5) * side / art - 0.5, sy + (y + 0.5) * side / art - 0.5)
            out[off + y][4 * (off + x):4 * (off + x) + 4] = bytes(px)
    return out


def make(exe, res):
    data = icon_png(exe)
    if not data:
        return False
    w, h, rows = png_decode(data)
    fg = foreground(w, h, rows)
    os.makedirs(os.path.join(res, 'drawable-nodpi'), exist_ok=True)
    with open(os.path.join(res, 'drawable-nodpi', 'ic_launcher_art.png'), 'wb') as f:
        f.write(png_encode(SIZE, SIZE, fg))
    os.makedirs(os.path.join(res, 'values'), exist_ok=True)
    with open(os.path.join(res, 'values', 'ic_launcher.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                '    <color name="ic_launcher_background">#FFFFFF</color>\n</resources>\n')
    os.makedirs(os.path.join(res, 'mipmap-anydpi-v26'), exist_ok=True)
    with open(os.path.join(res, 'mipmap-anydpi-v26', 'ic_launcher.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
                '    <background android:drawable="@color/ic_launcher_background" />\n'
                '    <foreground android:drawable="@drawable/ic_launcher_art" />\n'
                '</adaptive-icon>\n')
    return True


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(0 if make(sys.argv[1], sys.argv[2]) else 1)
