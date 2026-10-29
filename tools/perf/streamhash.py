#!/usr/bin/env python3
"""streamhash.py <run-dir> [--window MARK:COUNT] [--col full|p0|p1|p2|hdr|masked] [--ref <run-dir>] [--quiet]

Summarises <run>/stream.hash (written by nullgs.exe when NULLGS_HASH=1; columns in tools/perf/nullgs.c).
The sampled window is the RACE vblanks from the harness mark: stream frame index = game vblank - offset, where
offset = (game vblank at stop) - (frames in stream); both come from the run's mark file/stream.hash/log.
Prints one digest (sha256 over the per-vblank hash column of the window) + byte count; with --ref compares
per-vblank against another run and reports the first differing vblanks and which columns differ.
"""
import hashlib, os, re, sys
COLS = {'full': 3, 'p0': 4, 'p1': 5, 'p2': 6, 'hdr': 7, 'masked': 8}

def load(d):
    F = []
    tot = None
    for l in open(os.path.join(d, 'stream.hash')):
        x = l.split()
        if x and x[0] == 'F':
            F.append(x)
        elif x and x[0] == 'T':
            tot = x
    return F, tot

def window(d, F):
    mark = int(open(os.path.join(d, 'mark')).read().split()[0]) if os.path.exists(os.path.join(d, 'mark')) else 0
    stop = None
    for l in open(os.path.join(d, 'log.txt'), errors='replace'):
        m = re.search(r'\[HARNESS\] stop at vblank (\d+)', l)
        if m: stop = int(m.group(1))
    off = (stop - len(F)) if stop else 0
    return mark - off, (stop - off) if stop else len(F), mark, stop

def main():
    a = sys.argv[1:]
    d = a[0]; col = 'full'; ref = None; win = None
    i = 1
    while i < len(a):
        if a[i] == '--col': col = a[i + 1]; i += 2
        elif a[i] == '--ref': ref = a[i + 1]; i += 2
        elif a[i] == '--window': win = a[i + 1]; i += 2
        else: i += 1
    F, tot = load(d)
    lo, hi, mark, stop = window(d, F)
    if win:
        m, c = map(int, win.split(':')); lo, hi = m, m + c
    c = COLS[col]
    sel = F[lo:hi]
    h = hashlib.sha256(' '.join(x[c] for x in sel).encode()).hexdigest()[:16]
    nbytes = sum(int(x[2]) for x in sel)
    # Whole stream through the last vblank (the bytes after it are cut at an arbitrary point by the harness's stop).
    wh = hashlib.sha256(' '.join(x[3] for x in F).encode()).hexdigest()[:16]
    print(f'{d}: all {len(F)} vblanks, {sum(int(x[2]) for x in F)} bytes (+{tot[5]} unterminated tail ignored) digest={wh}')
    print(f'{d}: window frames {lo}..{hi - 1} (game vblank mark={mark} stop={stop}) col={col} bytes={nbytes} STREAM-HASH={h}')
    if ref:
        G, _ = load(ref)
        rlo, rhi, _, _ = window(ref, G)
        if win: rlo, rhi = lo, hi
        A, B = F[lo:hi], G[rlo:rhi]
        bad = [k for k in range(min(len(A), len(B))) if A[k][3:] != B[k][3:]]
        if len(A) != len(B): print(f'LENGTH differs: {len(A)} vs {len(B)} frames')
        if not bad and len(A) == len(B):
            print(f'IDENTICAL to {ref}: all {len(A)} per-vblank hashes match (every column)')
        else:
            print(f'DIFFERENT from {ref}: {len(bad)}/{len(A)} vblanks differ; first at window vblank {bad[0] if bad else "-"} '
                  f'(stream frame {lo + bad[0] if bad else "-"})')
            for k in bad[:5]:
                dc = [n for n, ci in COLS.items() if A[k][ci] != B[k][ci]]
                print(f'  vblank +{k}: bytes {A[k][2]} vs {B[k][2]}, columns differing: {",".join(dc)}')
        sys.exit(0 if not bad and len(A) == len(B) else 1)
main()
