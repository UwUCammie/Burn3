#!/usr/bin/env python3
"""stutter_report.py <profile-dir> [--exe <takedown-recomp.exe>] [--top N]

Reads a stutter profile recorded by a player's game (profiles/<date-time>/: frames.bin from the game, samples.bin,
stacks.txt, threads.txt and sampler.txt from tools/stutterprof.exe) and prints:
  - the frame rate over time (per second) and the slowest single frames (hitches),
  - per busy thread, its share of samples per frame in slow seconds against normal ones,
  - per thread, the functions that take more time per frame in slow seconds than in normal ones (exclusive: the
    innermost frame; inclusive: anywhere on the stack), with compiled game code (ee_fN) shown by guest address.
Slow seconds: gameplay seconds (under 200 fps) below 85% of the median gameplay frame rate; normal: the rest.
The exe defaults to the one named in info.txt (a Windows path inside the Wine prefix or the install).
"""
import bisect, collections, re, statistics, struct, subprocess, sys
from pathlib import Path

args = sys.argv[1:]
if not args:
    sys.exit(__doc__)
prof = Path(args[0])
top = int(args[args.index('--top') + 1]) if '--top' in args else 25
exe = Path(args[args.index('--exe') + 1]) if '--exe' in args else None
if exe is None:
    info = dict(l.split('=', 1) for l in (prof / 'info.txt').read_text().splitlines() if '=' in l)
    raw = info['exe']
    if re.match(r'^[A-Za-z]:\\', raw):   # Z:\home\... (Wine) or C:\... (the Wine prefix's drive_c)
        raw = raw[2:].replace('\\', '/') if raw[0] in 'Zz' else raw
    exe = Path(raw)

# --- symbols
syms = []
out = subprocess.run(['nm', '--defined-only', '-C', str(exe)], capture_output=True, text=True).stdout
for line in out.splitlines():
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[1] in 'tTwW':
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
sym_addr = [a for a, _ in syms]
guest = {}
gen = exe.parent / 'generated' / 'ee'
for inc in sorted(gen.glob('*.inc')) if gen.is_dir() else []:
    for m in re.finditer(r'\{(ee_f\d+), (0x[0-9a-fA-F]+)U', inc.read_text(errors='replace')):
        guest[m.group(1)] = int(m.group(2), 16)
image_base = int(re.search(r'base=([0-9a-f]+)', (prof / 'sampler.txt').read_text()).group(1), 16)
with open(exe, 'rb') as stream:   # the ImageBase nm's addresses are relative to (PE32+ optional header)
    head = stream.read(4096)
pe = struct.unpack_from('<I', head, 0x3c)[0]
link_base = struct.unpack_from('<Q', head, pe + 24 + 24)[0]
cache = {}
def name(rip):
    if rip in cache:
        return cache[rip]
    a = rip - image_base + link_base
    i = bisect.bisect_right(sym_addr, a) - 1
    if i < 0 or a - sym_addr[i] > 0x200000:
        n = '?' if not (image_base <= rip < image_base + 0x20000000) else 'exe+%x' % (rip - image_base)
    else:
        n = syms[i][1]
        m = re.search(r'ee_aot_detail::(ee_f\d+)\(', n)
        if m and m.group(1) in guest:
            n = 'guest %08x (AOT)' % guest[m.group(1)]
        if '{lambda' not in n:
            n = re.sub(r'\(.*', '', n)
    cache[rip] = n
    return n

# --- data
frames = [v for (v,) in struct.iter_unpack('<Q', (prof / 'frames.bin').read_bytes())]
stacks = {}
for line in (prof / 'stacks.txt').read_text().splitlines():
    i, fr = line.split('\t')
    stacks[int(i)] = [int(x, 16) for x in fr.split(',') if x]
samples = list(struct.iter_unpack('<QII', (prof / 'samples.bin').read_bytes()))
if len(frames) < 2 or not samples:
    sys.exit('no frames or no samples recorded')
t0 = frames[0]

# frame rate per second
per_sec = collections.Counter(int((f - t0) // 1_000_000_000) for f in frames)
secs = sorted(per_sec)
game = [s for s in secs if 10 <= per_sec[s] < 200]
med = statistics.median(per_sec[s] for s in game) if game else 0
slow = {s for s in game if per_sec[s] < 0.85 * med}
normal = set(game) - slow
print(f'{len(frames)} frames over {(frames[-1] - t0) / 1e9:.1f} s, {len(samples)} samples')
print(f'gameplay seconds {len(game)} (median {med:.0f} fps), slow seconds {len(slow)}')
print('\nfps per second (s:fps, * = slow):')
print(' '.join(f'{s}:{per_sec[s]}{"*" if s in slow else ""}' for s in secs))

# hitches: single frames over 2.5x the median frame time of their second
dts = [(frames[i + 1] - frames[i], i) for i in range(len(frames) - 1)]
hitches = []
for dt, i in dts:
    s = int((frames[i] - t0) // 1_000_000_000)
    if s in per_sec and per_sec[s] >= 10 and dt > 2.5e9 / per_sec[s] and dt > 25e6:
        hitches.append((dt, i))
hitches.sort(reverse=True)
print(f'\nhitches (frames over 2.5x their second\'s average and 25 ms): {len(hitches)}')
for dt, i in hitches[:15]:
    print(f'  {(frames[i] - t0) / 1e9:8.2f} s  {dt / 1e6:6.1f} ms')

# samples per thread and window class
threads = {}
for line in (prof / 'threads.txt').read_text().splitlines():
    tid, cpu, n = line.split('\t')
    threads[int(tid)] = (float(cpu), int(n))
def klass(ns):
    s = int((ns - t0) // 1_000_000_000)
    return 'slow' if s in slow else 'normal' if s in normal else None
frames_in = {'slow': sum(per_sec[s] for s in slow), 'normal': sum(per_sec[s] for s in normal)}
excl = collections.defaultdict(collections.Counter)
incl = collections.defaultdict(collections.Counter)
count = collections.Counter()
hitch_set = set()
for dt, i in hitches[:50]:
    hitch_set.add((frames[i], frames[i + 1]))
hitch_excl = collections.defaultdict(collections.Counter)
hitch_bounds = sorted(hitch_set)
hitch_starts = [a for a, _ in hitch_bounds]
for ns, tid, sid in samples:
    st = stacks.get(sid, [])
    if not st:
        continue
    k = klass(ns)
    leaf = name(st[0])
    if leaf == '?' and len(st) > 1:   # inside a system DLL: named by its caller in the game
        leaf = 'system code called from ' + name(st[1])
    j = bisect.bisect_right(hitch_starts, ns) - 1
    if j >= 0 and ns < hitch_bounds[j][1]:
        hitch_excl[tid][leaf] += 1
    if k is None:
        continue
    count[(tid, k)] += 1
    excl[(tid, k)][leaf] += 1
    for n in set(name(r) for r in st):
        incl[(tid, k)][n] += 1

busy = sorted({t for t, _ in count}, key=lambda t: -(count[(t, 'slow')] + count[(t, 'normal')]))
def label(tid):
    c = excl[(tid, 'normal')] + excl[(tid, 'slow')]
    named = [k for k, _ in c.most_common() if k != '?']
    return named[0][:70] if named else '?'
print('\nbusy threads: samples per frame, normal -> slow seconds (thread, CPU ms, top function)')
for tid in busy[:8]:
    a = count[(tid, 'normal')] / max(1, frames_in['normal'])
    b = count[(tid, 'slow')] / max(1, frames_in['slow'])
    print(f'  {tid:>6} {a:6.2f} -> {b:6.2f}  cpu {threads.get(tid, (0, 0))[0]:9.0f} ms  {label(tid)}')

def diff(table, tid, title):
    n_f, s_f = max(1, frames_in['normal']), max(1, frames_in['slow'])
    keys = set(table[(tid, 'normal')]) | set(table[(tid, 'slow')])
    rows = sorted(((table[(tid, 'slow')][k] / s_f - table[(tid, 'normal')][k] / n_f, k) for k in keys), reverse=True)
    print(f'    {title}: samples per frame, normal -> slow (largest increases)')
    for d, k in rows[:top]:
        if d <= 0:
            break
        print(f'      {table[(tid, "normal")][k] / n_f:7.3f} -> {table[(tid, "slow")][k] / s_f:7.3f}  {k[:110]}')

for tid in busy[:4]:
    print(f'\nthread {tid} ({label(tid)})')
    diff(excl, tid, 'exclusive')
    diff(incl, tid, 'inclusive')
    if hitch_excl[tid]:
        print('    in the worst hitch frames (exclusive samples):')
        for k, v in hitch_excl[tid].most_common(12):
            print(f'      {v:6d}  {k[:110]}')
