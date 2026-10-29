#!/usr/bin/env python3
"""inclusive.py <exe> <samples.tsv> --frames N --guest ee_aot_module.inc [options]

Inclusive/exclusive time per guest function and per game subsystem from the call stacks sampler.exe records
(<samples.tsv>.stk; SAMPLER_STACK_EVERY, default on).

How a stack is built. AOT guest calls are musttail jumps, so the HOST stack holds no guest nesting: it is
main -> run loop -> one ee_fN (the current guest function) -> helpers (VU0, native replacement, EE memory/DMA...).
The GUEST chain comes from the MIPS stack in EE RAM, walked by the sampler with per-function frame info that this
script writes (`--guest-table`, passed as SAMPLER_GUEST_TABLE). Here the two are spliced: dispatcher frames, the guest
chain root..current, then the host helper frames. The graphics thread has no guest code; its host stacks are classified
by VU1 microprogram / GIF arbiter / VIF handling.

  tools/perf/inclusive.py --guest <ee_aot_module.inc> --guest-table gtable.tsv       (once per kit build)
  SAMPLER_GUEST_TABLE=<windows path of gtable.tsv> tools/perf/perf.sh race NAME ; then
  tools/perf/inclusive.py <harness exe> <runs/NAME/samples.tsv> --guest <ee_aot_module.inc> --frames 3600 [--md out.md]

Options: --threads K (busiest K threads, default 2)  --top N  --tree DEPTH [--tree-min MS]  --fn 0xADDR  --under ADDR
ms/frame = thread CPU ms per frame x share of that thread's in-exe stack samples (symbolize.py's convention).
"""
import argparse, bisect, collections, glob, os, re, struct, subprocess, sys

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('exe', nargs='?'); ap.add_argument('samples', nargs='?')
ap.add_argument('--frames', type=int, default=3600)
ap.add_argument('--guest', help='ee_aot_module.inc of the build'); ap.add_argument('--threads', type=int, default=2)
ap.add_argument('--top', type=int, default=30)
ap.add_argument('--tree', type=int, default=0); ap.add_argument('--tree-min', type=float, default=0.05)
ap.add_argument('--fn', action='append', default=[]); ap.add_argument('--md')
ap.add_argument('--under', type=lambda x: int(x, 16), help='only stacks containing this guest function (hex)')
ap.add_argument('--guest-table', metavar='OUT', help='write the per-function frame table sampler.exe reads, and exit')
ap.add_argument('--elf', default=os.path.expanduser('~/Documents/games/Burnout 3/game/SLUS_210.50'))
args = ap.parse_args()
REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

guest = {}   # ee_fN -> (begin, end)
if args.guest:
    for m in re.finditer(r'\{(ee_f\d+), (0x[0-9a-fA-F]+)U, (0x[0-9a-fA-F]+)U', open(args.guest).read()):
        guest[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))

# ---------------------------------------------------------------- guest frame table for the sampler
if args.guest_table:
    elf = open(args.elf, 'rb').read()
    phoff, = struct.unpack_from('<I', elf, 0x1c); phentsize, phnum = struct.unpack_from('<HH', elf, 0x2a)
    segs = [struct.unpack_from('<IIIIIIII', elf, phoff + i * phentsize) for i in range(phnum)]
    segs = [(vaddr, off, fsz) for (t, off, vaddr, _, fsz, _, _, _) in segs if t == 1 and fsz]
    def word(a):
        for v, o, n in segs:
            if v <= a < v + n: return struct.unpack_from('<I', elf, o + a - v)[0]
        return 0
    rows = []
    for b, e in sorted(guest.values()):
        frame, raoff = 0, -1
        for a in range(b, min(e, b + 4 * 80), 4):     # prologue: addiu sp,sp,-N ... sd/sq/sw ra,off(sp)
            w = word(a)
            if (w >> 16) in (0x27bd, 0x67bd) and (w & 0x8000) and frame == 0: frame = 0x10000 - (w & 0xffff)
            elif (w >> 16) in (0xafbf, 0xffbf, 0x7fbf): raoff = w & 0xffff; break
            elif (w >> 26) in (2, 3) or w == 0x03e00008: break      # j / jal / jr ra before any ra save
        rows.append((b, e, frame if raoff >= 0 else 0, raoff))
    verify = b''.join(struct.pack('<I', word(0x100100 + 4 * i)) for i in range(4))   # identifies EE RAM in the child
    open(args.guest_table, 'w').write(f'V 100100 {verify.hex()}\n' + ''.join(f'{b:x} {e:x} {fr} {ro}\n' for b, e, fr, ro in rows))
    print(f'{len(rows)} functions, {sum(1 for r in rows if r[3] >= 0)} save ra'); sys.exit(0)
if not (args.exe and args.samples): ap.error('exe and samples are required')

# ---------------------------------------------------------------- names
# Subsystems: guest entry point -> (group, label). The innermost mapped function in a stack owns the sample, so a
# child (e.g. AI racers) carves its time out of its parent (world update). Found from the race call trees; labels
# marked (?) are inferred from the callees they reach, not from symbols (the game has none).
SUBSYS = {}
def S(group, label, *addrs):
    for a in addrs: SUBSYS[a] = (group, label)
S('Boot', 'main / boot / frame loop (other)', 0x100008, 0x12eb30, 0x132600)
S('Render', 'frame render driver (134a20/276300)', 0x134a20, 0x2768f0, 0x276300, 0x261e00)
S('Render', 'world/model draw submit (21c780, 21c120)', 0x21c780, 0x21c120, 0x332d00)
S('Render', 'effects / particles (3078f0, 151a40, 21e6f0)', 0x3078f0, 0x199a20, 0x151a40, 0x21e6f0, 0x301220)
S('Render', 'camera / projection (154350)', 0x154350, 0x1531c0)
S('HUD', 'HUD / 2D sprites (1a0790)', 0x1a0790)
S('Sim', 'world update (19a550, other)', 0x19a550)
S('Sim', 'physics step (1b8f00)', 0x1b8f00)
S('Sim', 'route / strip following + contact dispatch (143b10)', 0x143b10, 0x145a10)
S('Sim', 'AI racers (166160)', 0x166160)
S('Sim', 'damage / contacts / crash (?, 131f90)', 0x131f90, 0x28b160, 0x199dc0)
S('Sim', 'car state machines (?, 135180)', 0x135180, 0x15b000)
S('Sim', 'traffic (172e40)', 0x172e40, 0x18e874)
S('Spatial', 'spatial queries: broadphase / culling tree (301b00, 3be9b0)', 0x301b00, 0x301340, 0x3be9b0, 0x3bf720, 0x1c4890)
S('Input', 'input (214050)', 0x214050)
exec(open(os.environ['INCLUSIVE_MAP']).read()) if os.environ.get('INCLUSIVE_MAP') else None   # optional overrides

names = {}      # guest address -> function name where the sources document one
for f in glob.glob(os.path.join(REPO, 'src', '**', '*.cpp'), recursive=True):
    try: txt = open(f, errors='replace').read()
    except OSError: continue
    for m in re.finditer(r'//\s*([A-Za-z0-9_]+)[^\n]*?\(original (0x00[0-9a-fA-F]{6})\)', txt): names.setdefault(int(m.group(2), 16), m.group(1))
    for m in re.finditer(r'\{(0x00[0-9A-F]{6})U, 0x[0-9A-F]{8}U, "([a-z_0-9]+)"', txt): names.setdefault(int(m.group(1), 16), m.group(2))
def gname(a):
    g = SUBSYS.get(a); n = names.get(a)
    return f'{a:08x}' + (f' {n}' if n else '') + (f'  [{g[1]}]' if g and not n and False else '')

# Host-side categories for stacks (innermost frame first; first matching rule wins).
HOST_RULES = [
    (r'Spu2', 'Audio: SPU2 mixing'),
    (r'Slus21050CarProgram|before_vu1_launch', 'VU1: native car program'),
    (r'vu1_aot_function_(\d+)', None),
    (r'run_vu1_micro|vu1_aot|vu1_detail|vu1_chop', 'VU1: microprogram runner / helpers'),
    (r'pump_gif_arbiter|advance_gif|enqueue_gif|update_gif_arbiter|gif_dmac_can_progress', 'GIF arbiter / PATH1-3 handling'),
    (r'ParallelGsRawStreamWriter', 'GS stream writer (to renderer)'),
    (r'snapshot_vu1_xgkick|enqueue_vu1_path1', 'VU1 XGKICK / PATH1 packet snapshot'),
    (r'submit_vif1_word|transfer_pending_vif1|advance_vif1_dmac|run_graphics_ahead', 'VIF1 unpack + DMA tag handling'),
    (r'step_r5900|execute_r5900_instruction', 'Guest code run by the interpreter (AOT miss)'),
    (r'write_completed_command_dmac|capture_graphics_dmac|start_timed_vif1|service_graphics_dmacs|EeMemory', 'EE memory / DMA / GIF glue'),
    (r'NativeFunctionDispatcher|slus_21050|engine::|std::_Function_handler', 'Native function dispatch + replacements'),
    (r'run_vu0_micro|vu0_native|ee_vcallms', 'VU0 microprograms'),
]
HOST_RULES = [(re.compile(p), l) for p, l in HOST_RULES]
def host_label(frames_inner_first):
    for h in frames_inner_first:
        for rx, lab in HOST_RULES:
            m = rx.search(h)
            if m: return lab if lab else f'VU1: microprogram {m.group(1)}'
    return 'runner glue / other'
KIND_RULES = [('VU0', re.compile(r'run_vu0_micro|vu0_native|ee_vcallms|ee_cop2')),
              ('interp', re.compile(r'step_r5900|execute_r5900_instruction')),
              ('native', re.compile(r'NativeFunctionDispatcher|slus_21050|engine::|std::_Function_handler')),
              ('mem/DMA', re.compile(r'EeMemory|ee_store|ee_load|ee_callout|ee_exit'))]
def kind_of(host_inner):   # host frames below the innermost guest function
    for h in host_inner:
        for k, rx in KIND_RULES:
            if rx.search(h): return k
    return 'guest code'

# ---------------------------------------------------------------- host symbols
nm = subprocess.run(['nm', '--defined-only', args.exe], capture_output=True, text=True).stdout.splitlines()
raw = [l.split(' ', 2) for l in nm]; raw = [r for r in raw if len(r) == 3 and r[1] in 'tTwW']
dem = subprocess.run(['c++filt', '-p'], input='\n'.join(r[2] for r in raw), capture_output=True, text=True).stdout.splitlines()
syms = sorted((int(r[0], 16), d) for r, d in zip(raw, dem))
addrs = [a for a, _ in syms]
_nc = {}
def host_name(pc):
    r = _nc.get(pc)
    if r is None:
        i = bisect.bisect_right(addrs, pc) - 1
        r = syms[i][1] if i >= 0 and pc - addrs[i] < 0x400000 else '?'
        _nc[pc] = r
    return r
def classify(pc):
    n = host_name(pc)
    m = re.search(r'ee_aot_detail::(ee_f\d+)(?:[. ]|$)', n)
    if m and m.group(1) in guest: return ('G', guest[m.group(1)][0])
    n = re.sub(r'\.(part|constprop|isra|cold|lto_priv)\.?\d*', '', n)
    return ('H', n)

# ---------------------------------------------------------------- samples
cpu_ms = {}; flat = collections.Counter()
for line in open(args.samples):
    if line.startswith('# cpu '):
        _, _, tid, ms = line.split(); cpu_ms[tid] = float(ms)
    elif not line.startswith('#'):
        tid, rip, n = line.split(); flat[tid] += int(n)
stk_path = args.samples + '.stk'
if not os.path.exists(stk_path): sys.exit('no ' + stk_path + ' (sampler needs SAMPLER_STACK_EVERY>0)')
stacks = collections.defaultdict(list)
for line in open(stk_path):
    if line.startswith('#'): continue
    p = line.rstrip('\n').split('\t')
    gst = [int(x, 16) for x in p[3].split(',') if x] if len(p) > 3 else []
    stacks[p[0]].append((int(p[1]), [int(x, 16) for x in p[2].split(',')], gst))
nstack = {t: sum(x[0] for x in v) for t, v in stacks.items()}
busy = sorted(nstack, key=lambda t: -nstack[t])[:args.threads]
gstarts = sorted(v[0] for v in guest.values()); gend = {v[0]: v[1] for v in guest.values()}
def gfn_of(ra):
    i = bisect.bisect_right(gstarts, ra - 4) - 1
    return gstarts[i] if i >= 0 and ra - 4 < gend[gstarts[i]] else None
ROOT = 0x100008
ATTACH = re.compile(r'NativeFunctionDispatcher|step_r5900|execute_r5900|EeMemory::|service_graphics_dmacs|ee_vcallms|run_vu0_micro')

out_md = []
def emit(s=''):
    print(s); out_md.append(s)

def table(title, rows, cols, fmt=None):
    emit(f'\n### {title}'); emit('| ' + ' | '.join(cols) + ' |'); emit('|' + '|'.join(['---'] + ['---:'] * (len(cols) - 1)) + '|')
    for r in rows: emit('| ' + ' | '.join(r) + ' |')

for tid in busy:
    total = nstack[tid]; ms_thread = cpu_ms.get(tid, 0.0) / args.frames
    per = ms_thread / total
    conv = []   # (count, chain outer->inner of ('G', addr) | ('H', name))
    for n, fr, gst in stacks[tid]:
        chain = [classify(pc if k == 0 else pc - 1) for k, pc in enumerate(fr)][::-1]
        if gst:   # splice the guest chain (outer -> inner) in place of the innermost host ee_fN frame
            gi = max((i for i, (k, _) in enumerate(chain) if k == 'G'), default=None)
            up = [x for x in (gfn_of(ra) for ra in reversed(gst)) if x is not None]
            if gi is not None:
                f = chain[gi][1]
                if up and up[-1] == f: up.pop()
                chain[gi:gi + 1] = [('G', x) for x in up] + [('G', f)]
            elif up and not any(re.search('Spu2', v) for _, v in chain):
                # No ee_fN frame: a native replacement, the interpreter or EE memory/DMA code running for the guest.
                # The guest registers still describe its caller chain (audio and runner-only stacks are left alone).
                ai = next((i for i, (_, v) in enumerate(chain) if ATTACH.search(v)), None)
                if ai is not None: chain[ai:ai] = [('G', x) for x in up]
        conv.append((n, chain))
    # Chains the guest-stack walk could not finish (a saved ra it cannot read) stop short of main: complete them with
    # the most common ancestry that fully walked samples of their outermost function had.
    anc = collections.defaultdict(collections.Counter)
    gp = lambda ch: [v for k, v in ch if k == 'G']
    for n, ch in conv:
        g = gp(ch)
        if g and g[0] == ROOT:
            for i in range(len(g)): anc[g[i]][tuple(g[:i])] += n
    total_g = broken = fixed = 0
    for idx, (n, ch) in enumerate(conv):
        g = gp(ch)
        if not g: continue
        total_g += n
        if g[0] == ROOT: continue
        broken += n
        if g[0] in anc:
            pos = next(i for i, (k, _) in enumerate(ch) if k == 'G')
            conv[idx] = (n, ch[:pos] + [('G', v) for v in anc[g[0]].most_common(1)[0][0]] + ch[pos:]); fixed += n
    if args.under is not None: conv = [(n, ch) for n, ch in conv if ('G', args.under) in ch]
    emit(f'\n## thread {tid}: {ms_thread:.3f} ms/frame CPU; {total} stack samples = {100*total/max(1,flat[tid]):.0f}% of the thread\'s samples (the rest were outside the exe)')
    if total_g:
        emit(f'{100*total_g/total:.1f}% of the stacks contain guest code; {100*broken/total_g:.1f}% of those had an unreadable frame '
             f'(chain completed from the usual ancestry for {100*fixed/max(1,broken):.0f}% of them)')
    part = collections.defaultdict(collections.Counter)   # subsystem -> kind -> n
    incl_s = collections.Counter(); incl = collections.Counter(); excl = collections.Counter(); under = collections.Counter()
    host_incl = collections.Counter(); callers = collections.defaultdict(collections.Counter); callees = collections.defaultdict(collections.Counter)
    for n, chain in conv:
        gidx = [i for i, (k, _) in enumerate(chain) if k == 'G']
        host_inner = [v for k, v in chain[(gidx[-1] + 1 if gidx else 0):] if k == 'H'][::-1]
        host_all = [v for k, v in chain if k == 'H'][::-1]
        inner = next((v for k, v in reversed(chain) if k == 'G' and v in SUBSYS), None)
        if inner is not None: sub = SUBSYS[inner][0] + ': ' + SUBSYS[inner][1]
        elif gidx: sub = 'Guest code (unmapped)'
        else: sub = host_label(host_all)
        part[sub][kind_of(host_inner) if gidx else 'host'] += n
        seen = set()
        for k, v in chain:
            if k == 'G':
                if v not in seen: seen.add(v); incl[v] += n
                if v in SUBSYS: incl_s[SUBSYS[v][0] + ': ' + SUBSYS[v][1]] += n if (SUBSYS[v][0] + ': ' + SUBSYS[v][1]) not in seen else 0; seen.add(SUBSYS[v][0] + ': ' + SUBSYS[v][1])
            elif ('H', v) not in seen: seen.add(('H', v)); host_incl[v] += n
        if gidx:
            last = chain[gidx[-1]][1]
            (excl if gidx[-1] == len(chain) - 1 else under)[last] += n
        gs = [v for k, v in chain if k == 'G']
        for a, b in zip(gs, gs[1:]):
            if a != b: callers[b][a] += n; callees[a][b] += n
    f3 = lambda n: f'{n*per:.3f}'
    kinds = ['guest code', 'VU0', 'native', 'mem/DMA', 'interp', 'host']
    rows = [[s, f3(sum(c.values()))] + [f3(c[k]) for k in kinds] for s, c in sorted(part.items(), key=lambda x: -sum(x[1].values()))]
    rows.append(['**total**', f3(total)] + [f3(sum(c[k] for c in part.values())) for k in kinds])
    table('Subsystems, ms/frame (the innermost mapped guest function owns a sample, so rows add up to the thread total; '
          'columns say what kind of code the time is spent in)', rows, ['subsystem', 'ms/frame'] + ['guest code', 'VU0', 'native fn', 'EE mem/DMA', 'interp', 'host-only'])
    gr = collections.Counter()
    for s, c in part.items(): gr[s.split(': ')[0]] += sum(c.values())
    table('By group', [[g, f3(n), f'{100*n/total:.1f}%'] for g, n in gr.most_common()], ['group', 'ms/frame', '%'])
    table('Subsystems, inclusive (every mapped function a stack contains; rows overlap)',
          [[s, f3(n), f'{100*n/total:.1f}%'] for s, n in incl_s.most_common()], ['subsystem', 'incl ms/frame', '%'])
    if incl:
        table(f'Guest functions by inclusive time (top {args.top}); self = innermost frame is the function itself, glue = host code it called',
              [[gname(a), (SUBSYS[a][1] if a in SUBSYS else ''), f3(n), f3(excl[a]), f3(under[a])] for a, n in incl.most_common(args.top)],
              ['function', 'subsystem entry', 'incl ms/f', 'self ms/f', 'glue ms/f'])
        ex = collections.Counter(); [ex.update({a: excl[a] + under[a]}) for a in incl]
        table(f'Guest functions by exclusive time incl. their host glue (top {args.top})',
              [[gname(a), f3(n), f'{100*n/total:.2f}%'] for a, n in ex.most_common(args.top)], ['function', 'excl+glue ms/f', '%'])
    table('Host functions by inclusive time', [[h[:100], f3(n)] for h, n in host_incl.most_common(25)], ['function', 'incl ms/f'])
    for f in args.fn:
        a = int(f, 16)
        emit(f'\n### {gname(a)}: incl {incl[a]*per:.3f} self {excl[a]*per:.3f} glue {under[a]*per:.3f} ms/frame')
        emit('callers: ' + ', '.join(f'{gname(x)} {c*per:.3f}' for x, c in callers[a].most_common(8)))
        emit('callees: ' + ', '.join(f'{gname(x)} {c*per:.3f}' for x, c in callees[a].most_common(12)))
    if args.tree:
        root = {'_n': 0}
        for n, chain in conv:
            node = root; gs = [v for k, v in chain if k == 'G']
            prev = None
            for v in gs[:args.tree]:
                if v == prev: continue
                prev = v; node = node.setdefault(v, {'_n': 0}); node['_n'] += n
        def dump(node, depth):
            for k in sorted((k for k in node if k != '_n'), key=lambda k: -node[k]['_n']):
                ms = node[k]['_n'] * per
                if ms < args.tree_min: continue
                lab = SUBSYS[k][1] if k in SUBSYS else ''
                emit('  ' * depth + f'{ms:7.3f}  {gname(k)}  {lab}'); dump(node[k], depth + 1)
        emit(f'\n### guest call tree (ms/frame inclusive, depth {args.tree}, pruned below {args.tree_min} ms)'); dump(root, 0)

if args.md: open(args.md, 'w').write('\n'.join(out_md) + '\n')
