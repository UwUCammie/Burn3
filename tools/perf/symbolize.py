#!/usr/bin/env python3
"""symbolize.py <exe> <samples.tsv> [top N] [--guest generated/ee/ee_aot_module.inc]: flat profile by symbol."""
import bisect, collections, re, subprocess, sys
exe, samples = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 and sys.argv[3].isdigit() else 60
guest = {}
if '--guest' in sys.argv:
    inc = sys.argv[sys.argv.index('--guest') + 1]
    for m in re.finditer(r'\{(ee_f\d+), (0x[0-9a-fA-F]+)U, (0x[0-9a-fA-F]+)U', open(inc).read()):
        guest[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
syms = []
nm_out = subprocess.run(['nm', '--defined-only', exe], capture_output=True, text=True).stdout.splitlines()
raw = [l.split(' ', 2) for l in nm_out]
raw = [r for r in raw if len(r) == 3]
dem = subprocess.run(['c++filt', '-p'], input='\n'.join(r[2] for r in raw), capture_output=True, text=True).stdout.splitlines()
for r, d in zip(raw, dem):
    line = r[0] + ' ' + r[1] + ' ' + d
    parts = line.split(' ', 2)
    if len(parts) == 3 and parts[1] in 'tTwW':
        syms.append((int(parts[0], 16), parts[2]))
syms.sort(); addrs = [a for a, _ in syms]
by_sym = collections.Counter(); by_thread = collections.Counter(); total = 0
cpu_ms = {}; wall_ms = 0
for line in open(samples):
    if line.startswith('# cpu '):
        _, _, tid, ms = line.split(); cpu_ms[tid] = float(ms); continue
    if line.startswith('# samples='):
        m = re.search(r'wall_ms=(\d+)', line); wall_ms = int(m.group(1)) if m else 0; continue
    if line.startswith('#'): continue
    tid, rip, n = line.split(); rip = int(rip, 16); n = int(n)
    total += n; by_thread[tid] += n
    i = bisect.bisect_right(addrs, rip) - 1
    name = syms[i][1] if i >= 0 and rip - addrs[i] < 0x200000 else '[outside exe: wine/dll/kernel]'
    m = re.search(r'ee_aot_detail::(ee_f\d+)$', name)
    if m and m.group(1) in guest:
        b, e = guest[m.group(1)]; name = f'GUEST {m.group(1)} [0x{b:08x}-0x{e:08x}]'
    by_sym[(tid, name)] += n
print(f'total samples {total}')
inexe = collections.Counter()
for (tid, name), n in by_sym.items():
    if not name.startswith('[outside'): inexe[tid] += n
busy = [t for t, n in inexe.most_common() if n > 0.05 * by_thread[t]]
frames = int(sys.argv[sys.argv.index('--frames') + 1]) if '--frames' in sys.argv else 0
for tid, n in by_thread.most_common():
    extra = ''
    if tid in cpu_ms:
        extra = f', cpu {cpu_ms[tid]:.0f} ms'
        if wall_ms: extra += f' ({100*cpu_ms[tid]/wall_ms:.1f}% of a core)'
        if frames: extra += f', {cpu_ms[tid]/frames:.3f} ms/frame'
    print(f'thread {tid}: {n} samples, {100*inexe[tid]/n:.1f}% in exe{extra}')
if cpu_ms and frames: print(f'process cpu: {sum(cpu_ms.values())/frames:.3f} ms/frame over {frames} frames')
for tid in busy[:3]:
    tt = by_thread[tid]; print(f'\n== thread {tid} top ==')
    c = collections.Counter({name: n for (t, name), n in by_sym.items() if t == tid})
    for name, n in c.most_common(top): print(f'{100*n/tt:6.2f}%  {n:7d}  {name[:160]}')
main_tid = by_thread.most_common(1)[0][0]
print(f'\n== top symbols, all threads ==')
agg = collections.Counter()
for (tid, name), n in by_sym.items(): agg[name] += n
for name, n in agg.most_common(top): print(f'{100*n/total:6.2f}%  {n:7d}  {name[:160]}')
g = sum(n for name, n in agg.items() if name.startswith('GUEST'))
print(f'\nAOT guest code total: {100*g/total:.2f}%')
