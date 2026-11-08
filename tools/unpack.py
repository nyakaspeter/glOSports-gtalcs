#!/usr/bin/env python3
"""The Android Stories engine's OBBs unpacked into a folder, for the
loader's game/files/ (loader/game.c: the game opens game/files/<name> and
needs no OBB).

  unpack.py <out dir> <libGTALcs.so> <names...> -- <main.obb> [<patch.obb>...]

The OBBs store files by hash only (~crc32 of the lower-case path,
tools/wad.py), so names are recovered: every texture the OBBs' own
master_texture_mapping_process.csv names (Textures_ETC*/<txd>/<tex>.pvr),
the path-like strings of libGTALcs.so and its path formats filled with its
words and numbers (radio stations...), the same from every file named so
far (gta_vc.dat names the map's files...) until nothing new turns up, and
the names files given (a path
per line; the loader's 'file: <name>' log lines, GTALCS_FILELOG=1, also
work). Later OBBs win (the patch over the main). Files are written under
their names as found (HOME on the console is vfat: case doesn't matter).
Files that stay unnamed go to <out dir>/_byhash/<hash, 8 hex digits>, where
the loader looks for a name it doesn't find (by the same hash); <out
dir>.unnamed.txt lists them (hash, size).
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wad

TEXTURE_DIRS = ('Textures_ETC', 'Textures_ETC_LOD')
EXTENSIONS = ('ide', 'ipl', 'col', 'dff', 'txd', 'img', 'dir', 'lvz', 'xml', 'dat', 'txt', 'png',
              'pvr', 'mp3', 'wav', 'ifp', 'cut', 'IPL', 'IDE', 'COL', 'DFF', 'TXD', 'MP3')


def candidates(wads, so, names_files):
    """-> (names seen as such, names guessed from formats, the formats).
    Guesses are many (formats x words): some match a real file's 32-bit
    hash by chance, so a name seen anywhere always wins."""
    out, guessed = set(), set()
    for w in wads:
        for csv in ('master_texture_mapping_process.csv', 'master_missing_textures_process.csv'):
            e = w.find(csv)
            if not e:
                continue
            out.add(csv)
            for l in w.data(e).decode('latin1').splitlines():
                f = l.split(',')
                if len(f) >= 3:
                    for sub in TEXTURE_DIRS:
                        out.add('%s/%s/%s.pvr' % (sub, f[1].strip(), f[2].strip()))
    data = open(so, 'rb').read()
    out |= paths_in(data)
    # names built at run time: the path formats (one %s or %d) filled with
    # every word of the library (radio stations, levels...) and numbers
    formats = [m.group().decode().replace('\\', '/') for m in re.finditer(rb'[A-Za-z0-9_./\\%-]{3,100}', data)]
    formats = [f for f in formats if f.count('%') == 1 and ('/' in f or '.' in f)]
    guessed |= filled(formats, words_in(data))
    for f in formats:
        if re.search(r'%0?\d?d', f):
            guessed.update(re.sub(r'%0?\d?d', lambda _: str(k), f).lstrip('/') for k in range(1000))
            guessed.update(re.sub(r'%0?\d?d', lambda _: '%02d' % k, f).lstrip('/') for k in range(100))
    for path in names_files:
        for l in open(path, encoding='latin1'):
            l = l.strip()
            if not l or l.startswith('#'):
                continue
            m = re.search(r'file: (.*?)( \(none\))?$', l)
            out.add((m.group(1) if m else l).replace('\\', '/').lstrip('/'))
    return out, guessed, formats


def paths_in(data):
    out = set()
    for m in re.finditer(rb'[A-Za-z0-9_./\\ -]{4,100}', data):
        s = m.group().decode().replace('\\', '/').strip()
        if '.' in s:
            for x in [s] + s.split():
                out.add(x)
                out.add(x.lstrip('/'))
    return out


def words_in(data):
    return set(m.group().decode() for m in re.finditer(rb'[A-Za-z0-9_]{1,24}', data))


def filled(formats, words):
    return set(f.replace('%s', w).lstrip('/') for f in formats if '%s' in f for w in words)


def main(a):
    if '--' not in a or a.index('--') < 2:
        sys.exit(__doc__)
    k = a.index('--')
    out, so, names_files, obbs = a[0], a[1], a[2:k], a[k + 1:]
    wads = [wad.Wad(p) for p in obbs]
    seen_names, guessed, formats = candidates(wads, so, names_files)
    names = {wad.path_hash(c): c for c in guessed}
    names.update({wad.path_hash(c): c for c in seen_names})
    sure = set(wad.path_hash(c) for c in seen_names)
    final = {}
    for w in wads:          # later wins
        for e in w.fat:
            final[e[0]] = (w, e)
    # the named files name others (gta_vc.dat its IDEs, IPLs and COLs; the
    # script and cutscene directory their sounds): until nothing new
    seen = set()
    while True:
        new, new_guessed = set(), set()
        for h, (w, e) in final.items():
            if h in names and h not in seen and e[5] < 4 << 20:
                seen.add(h)
                d = w.data(e)
                new |= paths_in(d)
                new_guessed |= filled(formats, words_in(d))
        # and every name under the other extensions (an IDE's IPL and COL...)
        for n in names.values():
            base = n.rsplit('.', 1)[0]
            new.update(base + '.' + x for x in EXTENSIONS)
        found = {wad.path_hash(c): c for c in new_guessed if wad.path_hash(c) in final and wad.path_hash(c) not in names}
        found.update({wad.path_hash(c): c for c in new if wad.path_hash(c) in final and wad.path_hash(c) not in sure})
        found = {h: c for h, c in found.items() if names.get(h) != c}
        if not found:
            break
        names.update(found)
        sure |= set(wad.path_hash(c) for c in new if wad.path_hash(c) in found)
    unnamed, written = [], 0
    for h, (w, e) in sorted(final.items()):
        name = names.get(h)
        if not name:
            unnamed.append((h, e[5]))
            name = '_byhash/%08x' % h       # the loader looks here by the name's hash
        dst = os.path.join(out, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(w.data(e))
        written += 1
    with open(out.rstrip('/') + '.unnamed.txt', 'w') as f:
        for h, n in unnamed:
            f.write('%08x %d\n' % (h, n))
    print('%s: %d files, %d unnamed (%.1f MB)' % (out, written, len(unnamed),
                                                  sum(n for h, n in unnamed) / 1e6))


if __name__ == '__main__':
    main(sys.argv[1:])
