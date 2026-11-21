"""Print the Matroska Video/Colour elements of the first video track (S0 probe, measure only)."""
import sys

def vint(d, i, strip):
    b = d[i]; n = 1; m = 0x80
    while n <= 8 and not (b & m):
        n += 1; m >>= 1
    v = (b & (m - 1)) if strip else b
    for k in range(1, n):
        v = (v << 8) | d[i + k]
    return v, n

NAMES = {0x55B1: 'MatrixCoefficients', 0x55B9: 'Range', 0x55B7: 'ChromaSitingHorz', 0x55B8: 'ChromaSitingVert',
         0x55BA: 'TransferCharacteristics', 0x55BB: 'Primaries', 0x55B2: 'BitsPerChannel'}
MASTERS = {0x18538067, 0x1654AE6B, 0xAE, 0xE0, 0x55B0}  # Segment, Tracks, TrackEntry, Video, Colour

def walk(d, i, end, out):
    while i < end:
        eid, n = vint(d, i, False); i += n
        size, n = vint(d, i, True); i += n
        if eid == 0x1F43B675:  # Cluster: stop
            return True
        if eid in MASTERS:
            if walk(d, i, min(end, i + size), out):
                return True
        elif eid in NAMES:
            out[NAMES[eid]] = int.from_bytes(d[i:i + size], 'big')
        i += size
    return False

for f in sys.argv[1:]:
    d = open(f, 'rb').read(1 << 20)
    eid, n = vint(d, 0, False); size, m = vint(d, n, True)
    out = {}
    walk(d, n + m + size, len(d), out)
    print(f.replace(chr(92), '/').split('/')[-1], out or '(no Colour element)')
