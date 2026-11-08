#!/usr/bin/env python3
"""The Android Stories engine's WAD archives (the .obb files), as libGTALcs.so
reads them (WadArchive::OpenWad, SerialiseEncryptDecryptBuffer, FSWadFile).

  wad.py list    <obb> [names.txt]           hash, size, name (if known)
  wad.py extract <obb> <outdir> <names.txt>  every file whose name is known
  wad.py get     <obb> <path> <dest>         one file by its path
  wad.py pack    <srcdir> <obb> [base.obb[,base2.obb…]]
                                             every file under srcdir, by its relative path
                                             (hard links stored once),
                                             plus every file of the base WADs (by hash, the
                                             first base winning) that srcdir does not replace

Format: the whole file is XORed with SerialiseEncryptionKey's default key
AF 66, by absolute offset (key[offset & 1]). At 0: 'LWAD', u32 1, u32 1,
padded to 0x800. At 0x800: u32 nDirs, nDirs x {i32 parent, u32 nameOffset};
u32 nFat, nFat x 48-byte entries {u32 pathHash, u32 nameOffset, u32 dir,
u32 crc, u64 offset, u64 size, u64 size, u64 0} sorted by pathHash (bsearch);
u32 stringTableSize + strings (empty in Rockstar's OBBs: files are found by
hash only). pathHash = ~crc32(lowercase path) (Hash_CRC32Calc: init ~0, no
final inversion). crc = CRC-32 of the data with init 0 and no inversion,
XOR 0x61EA4E00 (FSWadFile::IsCRCValid).
"""
import os, struct, sys, zlib

KEY = b'\xaf\x66'
M = 0xffffffff
FAT_AT = 0x800


def xor(data, off):
    # XOR with the 2-byte key by absolute offset, fast for large blocks.
    k = KEY if off % 2 == 0 else KEY[::-1]
    n = len(data)
    key = (k * (n // 2 + 1))[:n]
    return (int.from_bytes(data, 'little') ^ int.from_bytes(key, 'little')).to_bytes(n, 'little')


def path_hash(path):
    return ~zlib.crc32(path.lower().encode('latin1')) & M


def file_crc(data):
    return (~zlib.crc32(data, M) & M) ^ 0x61EA4E00


class Wad:
    def __init__(self, path):
        self.f = open(path, 'rb')
        self.f.seek(0, 2)
        size = self.f.tell()
        head = self.read(0, 12)
        if head[:4] != b'DAWL':
            raise SystemExit('%s: not a WAD' % path)
        blob = self.read(FAT_AT, min(size - FAT_AT, 16 << 20))
        p = 0
        nd, = struct.unpack_from('<I', blob, p); p += 4 + 8 * nd
        nf, = struct.unpack_from('<I', blob, p); p += 4
        self.fat = [struct.unpack_from('<IIIIQQQQ', blob, p + 48 * i) for i in range(nf)]
        self.by_hash = {e[0]: e for e in self.fat}

    def read(self, off, n):
        self.f.seek(off)
        return xor(self.f.read(n), off)

    def data(self, e):
        return self.read(e[4], e[5])

    def find(self, path):
        return self.by_hash.get(path_hash(path))


def load_names(path):
    names = {}
    if path:
        for l in open(path, encoding='latin1'):
            l = l.strip()
            if l and not l.startswith('#'):
                names[path_hash(l)] = l
    return names


def pack(src, out, base=None):
    files = []
    for root, _, fs in os.walk(src):
        for fn in fs:
            full = os.path.join(root, fn)
            files.append((os.path.relpath(full, src).replace(os.sep, '/'), full))
    entries = [(path_hash(rel), rel, full) for rel, full in files]
    for bp in (base.split(',') if base else []):
        b, have = Wad(bp), set(e[0] for e in entries)
        entries += [(e[0], '%08x' % e[0], (b, e)) for e in b.fat if e[0] not in have]
    entries.sort(key=lambda x: x[0])
    for a, b in zip(entries, entries[1:]):
        if a[0] == b[0]:
            raise SystemExit('hash collision: %s %s' % (a[1], b[1]))
    fat_size = 4 + 8 + 4 + 48 * len(entries) + 4
    off = (FAT_AT + fat_size + 0x7ff) & ~0x7ff
    fat = bytearray()
    stored = {}         # (device, inode) -> (offset, size, crc): hard links are stored once
    with open(out, 'wb') as o:
        o.write(b'\0' * off)
        for h, rel, full in entries:
            if not isinstance(full, tuple):
                st = os.stat(full)
                if (st.st_dev, st.st_ino) in stored:
                    at, size, crc = stored[(st.st_dev, st.st_ino)]
                    fat += struct.pack('<IIIIQQQQ', h, 0, 0, crc, at, size, size, 0)
                    continue
            if isinstance(full, tuple):
                # copy a base entry in 64 MB pieces (the big IMGs and the audio)
                w0, e = full
                n, src, dst = e[5], e[4], off
                while n:
                    k = min(n, 64 << 20)
                    o.seek(dst)
                    o.write(xor(w0.read(src, k), dst))
                    n, src, dst = n - k, src + k, dst + k
                d = None
            else:
                d = open(full, 'rb').read()
                o.seek(off)
                o.write(xor(d, off))
            if d is None:
                size, crc = full[1][5], full[1][3]     # unchanged data keeps its CRC
            else:
                size, crc = len(d), file_crc(d)
                st = os.stat(full)
                stored[(st.st_dev, st.st_ino)] = (off, size, crc)
            fat += struct.pack('<IIIIQQQQ', h, 0, 0, crc, off, size, size, 0)
            off = (off + size + 0x7ff) & ~0x7ff
        o.truncate(off)
        head = struct.pack('<4sII', b'DAWL', 1, 1).ljust(FAT_AT, b'\0')
        table = struct.pack('<IiI', 1, -1, 0) + struct.pack('<I', len(entries)) + fat + struct.pack('<I', 0)
        o.seek(0)
        o.write(xor(head, 0))
        o.write(xor(table, FAT_AT))
    print('%s: %d files, %d bytes' % (out, len(entries), off))


def main(a):
    if len(a) >= 2 and a[0] == 'list':
        w, names = Wad(a[1]), load_names(a[2] if len(a) > 2 else None)
        for e in w.fat:
            print('%08x %11d %s' % (e[0], e[5], names.get(e[0], '?')))
    elif len(a) == 4 and a[0] == 'extract':
        w, names = Wad(a[1]), load_names(a[3])
        for e in w.fat:
            if e[0] in names:
                dst = os.path.join(a[2], names[e[0]])
                os.makedirs(os.path.dirname(dst) or '.', exist_ok=True)
                open(dst, 'wb').write(w.data(e))
    elif len(a) == 4 and a[0] == 'get':
        w = Wad(a[1])
        e = w.find(a[2]) or sys.exit('%s: not in %s' % (a[2], a[1]))
        open(a[3], 'wb').write(w.data(e))
    elif len(a) in (3, 4) and a[0] == 'pack':
        pack(a[1], a[2], a[3] if len(a) > 3 else None)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
