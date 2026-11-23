"""Apply FINAL FANTASY XIV 1.x patch files (ZiPatch, D<version>.patch) to an install, without the
game's own updater.
  python3 tools/xiv_patch.py <install folder> <boot|game> <patch file or folder>... [--check]

For servers whose patch service the native ffxivboot cannot use (it downloads only signed
torrents): fetch the .patch files over HTTP however the server offers them, then apply them here.
Patches at or below the install's boot.ver or game.ver are skipped; the rest are applied in
version order, and the .ver file is written after each one, so a second run resumes.

The format, as the files have it (all integers big-endian):
  the 12-byte magic 91 'ZIPATCH' 0d 0a 1a 0a, then chunks: u32 size, 4-byte type, body, u32 crc.
  FHDR, APLY  header and apply options, nothing to do here
  ADIR        u32 length, path: make the folder
  DELD        u32 length, path: remove the folder (if empty)
  ETRY        u32 length, path, u32 count, then count blocks of
              u32 mode ('A' add, 'M' modify, 'D' delete, in the top byte), 20-byte SHA-1 of the file
              before, 20-byte SHA-1 after, u32 compression ('Z' zlib or 'N' none, top byte),
              u32 data size, u32 size before, u32 size after, data.
Each block that has data holds the whole file after it, not a difference. The blocks before the
last record the versions the file went through; the last one says what the file becomes. A file
is replaced only when its current SHA-1 is one the entry starts from (or it is already the result),
and the result is checked against the entry's SHA-1 before it goes in place.

--check reads and verifies every block of the patches without writing anything.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
import zlib

MAGIC = b'\x91ZIPATCH\r\n\x1a\n'
NO_FILE = 0  # size before of a block that adds a file


class PatchError(Exception):
    pass


def chunks(path):
    """(type, body) for each chunk of a patch file"""
    with open(path, 'rb') as f:
        if f.read(12) != MAGIC:
            raise PatchError('%s: not a ZiPatch file' % path)
        while True:
            head = f.read(8)
            if not head:
                return
            if len(head) < 8:
                raise PatchError('%s: truncated chunk header' % path)
            size, kind = struct.unpack('>I', head[:4])[0], head[4:]
            body = f.read(size)
            if len(body) < size or len(f.read(4)) < 4:
                raise PatchError('%s: truncated %s chunk' % (path, kind.decode(errors='replace')))
            yield kind, body


def read_path(body, at=0):
    n, = struct.unpack('>I', body[at:at + 4])
    return body[at + 4:at + 4 + n].split(b'\0', 1)[0].decode('ascii'), at + 4 + n


def entry_blocks(body):
    """the path of an ETRY chunk and its blocks: (mode, before sha1, after sha1, size after, data)"""
    path, p = read_path(body)
    count, = struct.unpack('>I', body[p:p + 4])
    p += 4
    blocks = []
    for _ in range(count):
        mode = chr(body[p])
        before, after = body[p + 4:p + 24], body[p + 24:p + 44]
        comp, csize, _, nsize = struct.unpack('>4I', body[p + 44:p + 60])
        data = body[p + 60:p + 60 + csize]
        p += 60 + csize
        if csize:
            kind = chr(comp >> 24)
            if kind == 'Z':
                data = zlib.decompress(data)
            elif kind != 'N':
                raise PatchError('%s: unknown compression %#x' % (path, comp))
            if len(data) != nsize or hashlib.sha1(data).digest() != after:
                raise PatchError('%s: block data does not match its SHA-1' % path)
        else:
            data = None
        if mode not in 'AMD':
            raise PatchError('%s: unknown mode %r' % (path, mode))
        blocks.append((mode, before, after, nsize, data))
    if p != len(body):
        raise PatchError('%s: %d bytes left over in its entry' % (path, len(body) - p))
    return path, blocks


def sha1_file(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for piece in iter(lambda: f.read(1 << 20), b''):
            h.update(piece)
    return h.digest()


def apply_entry(install, path, blocks):
    """bring one file to the state the entry's last block gives; returns what was done"""
    target = os.path.join(install, *path.split('/'))
    mode, _, final, nsize, data = blocks[-1]
    have = sha1_file(target) if os.path.isfile(target) else None
    if mode == 'D':
        if have is None:
            return 'absent'
    elif have == final:
        return 'current'
    starts = {b[1] for b in blocks}
    adds = any(b[0] == 'A' for b in blocks)
    if have is not None and have not in starts and not adds:
        raise PatchError('%s: the file is not a version this patch starts from' % path)
    if mode == 'D':
        os.remove(target)
        return 'deleted'
    if data is None:
        if nsize:
            raise PatchError('%s: the last block has no data' % path)
        data = b''
        if hashlib.sha1(data).digest() != final:
            raise PatchError('%s: an empty result does not match its SHA-1' % path)
    os.makedirs(os.path.dirname(target), exist_ok=True)
    tmp = target + '.xivpatch'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, target)
    return 'added' if have is None else 'replaced'


def version_of(path):
    m = re.search(r'D(\d{4}\.\d\d\.\d\d\.\d{4})\.patch$', os.path.basename(path))
    if not m:
        raise PatchError('%s: the name is not D<version>.patch' % path)
    return m.group(1)


def patch_files(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += [os.path.join(a, n) for n in os.listdir(a) if n.endswith('.patch')]
        else:
            out.append(a)
    return sorted(out, key=version_of)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('install', help='the FINAL FANTASY XIV folder')
    ap.add_argument('repo', choices=('boot', 'game'))
    ap.add_argument('patches', nargs='+', help='D<version>.patch files, or folders of them')
    ap.add_argument('--check', action='store_true', help='verify the patches, write nothing')
    a = ap.parse_args()
    install = os.path.abspath(a.install)
    ver_file = os.path.join(install, a.repo + '.ver')
    current = open(ver_file).read().strip() if os.path.exists(ver_file) else '0000.00.00.0000'
    todo = [p for p in patch_files(a.patches) if a.check or version_of(p) > current]
    if not todo:
        print('%s is at %s: nothing to apply' % (a.repo, current))
        return
    try:
        for p in todo:
            v = version_of(p)
            tally = {}
            for kind, body in chunks(p):
                if kind in (b'FHDR', b'APLY'):
                    continue
                if kind == b'ETRY':
                    path, blocks = entry_blocks(body)
                    done = 'checked' if a.check else apply_entry(install, path, blocks)
                elif kind in (b'ADIR', b'DELD'):
                    path, _ = read_path(body)
                    folder = os.path.join(install, *path.split('/'))
                    if a.check:
                        done = 'checked'
                    elif kind == b'ADIR':
                        os.makedirs(folder, exist_ok=True)
                        done = 'folder'
                    else:
                        try:
                            os.rmdir(folder)
                            done = 'folder removed'
                        except OSError:
                            done = 'folder kept'
                else:
                    raise PatchError('%s: unknown chunk %r' % (p, kind))
                tally[done] = tally.get(done, 0) + 1
            if not a.check:
                with open(ver_file, 'w') as f:
                    f.write(v)
            print('%s %s: %s' % (a.repo, v, ', '.join('%d %s' % (n, k) for k, n in sorted(tally.items()))), flush=True)
    except PatchError as e:
        raise SystemExit('stopped: %s' % e)


if __name__ == '__main__':
    try:
        main()
    except PatchError as e:
        raise SystemExit('stopped: %s' % e)
