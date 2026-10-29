// Minimal Windows sampling profiler: sampler.exe <out.tsv> <interval_us> <delay_s> -- <command line...>
// Starts the child, waits delay_s, then samples RIP of every thread of the child until it exits.
// Output: tid \t rip(hex) \t count, plus a "# samples" header with per-thread totals.
// SAMPLER_STACK_EVERY=N (default 3, 0 = off) also writes call stacks to <out>.stk (see capture_stack).
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { DWORD tid; unsigned long long rip; unsigned count; } Entry;
static Entry* table; static size_t cap = 1u << 22, used;
// Per-thread CPU time (user+kernel, 100 ns units) from when sampling starts.
typedef struct { DWORD tid; unsigned long long start, last; } Cpu;
static Cpu cpus[512]; static int ncpus;
static unsigned long long thread_cpu(HANDLE t) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(t, &c, &e, &k, &u)) return 0;
    return (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);
}
static void note_cpu(DWORD tid, HANDLE t) {
    unsigned long long v = thread_cpu(t);
    for (int i = 0; i < ncpus; ++i) if (cpus[i].tid == tid) { if (v > cpus[i].last) cpus[i].last = v; return; }
    if (ncpus < 512) { cpus[ncpus].tid = tid; cpus[ncpus].start = v; cpus[ncpus].last = v; ++ncpus; }
}

static void add(DWORD tid, unsigned long long rip) {
    size_t h = (size_t)((rip * 0x9E3779B97F4A7C15ull) ^ tid) & (cap - 1);
    for (;;) {
        if (table[h].count == 0) { table[h].tid = tid; table[h].rip = rip; table[h].count = 1; used++; return; }
        if (table[h].tid == tid && table[h].rip == rip) { table[h].count++; return; }
        h = (h + 1) & (cap - 1);
    }
}

// ---- Call stacks (SAMPLER_STACK_EVERY=N: every N-th sample of a thread that is executing inside the exe) ----
// x64 unwinder driven by the exe's own .pdata/UNWIND_INFO, read from the child's memory (no dbghelp). The thread is
// held suspended while its stack is copied and walked. Output: <out>.stk with one line per distinct stack:
//   tid \t count \t rip,ret1,ret2,... (hex, innermost first; stops where the chain leaves the exe image)
typedef struct { unsigned begin, end, unwind; } RF;
static HANDLE g_proc; static unsigned long long img_base, img_size; static RF* pdata; static size_t npdata;
static unsigned char* slab; static unsigned long long slab_lo, slab_hi;
static unsigned char** uw_cache_k; static unsigned* uw_keys; static size_t uw_cap = 1u << 17;
typedef struct { DWORD tid; unsigned long long hi; } StackTop;
static StackTop stack_tops[512]; static int nstack_tops;
typedef LONG (NTAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);

static int rpm(unsigned long long a, void* dst, size_t n) {
    SIZE_T got = 0; return ReadProcessMemory(g_proc, (LPCVOID)a, dst, n, &got) && got == n;
}
static int rd64(unsigned long long a, unsigned long long* v) {
    if (a >= slab_lo && a + 8 <= slab_hi) { memcpy(v, slab + (a - slab_lo), 8); return 1; }
    return rpm(a, v, 8);
}
static int load_image(unsigned long long base) {
    unsigned char hdr[0x1000];
    if (!rpm(base, hdr, sizeof hdr) || hdr[0] != 'M') return 0;
    unsigned pe = *(unsigned*)(hdr + 0x3c);
    if (pe + 0x120 > sizeof hdr) return 0;
    unsigned char* opt = hdr + pe + 24;
    img_base = base; img_size = *(unsigned*)(opt + 56);
    unsigned prva = *(unsigned*)(opt + 112 + 3 * 8), psz = *(unsigned*)(opt + 112 + 3 * 8 + 4);
    if (!prva || !psz) return 0;
    pdata = malloc(psz); npdata = psz / sizeof(RF);
    if (!rpm(base + prva, pdata, psz)) return 0;
    uw_cache_k = calloc(uw_cap, sizeof *uw_cache_k); uw_keys = calloc(uw_cap, sizeof *uw_keys);
    slab = malloc(1u << 20);
    return 1;
}
static const RF* find_rf(unsigned long long pc) {
    if (pc < img_base || pc >= img_base + img_size) return NULL;
    unsigned rva = (unsigned)(pc - img_base); size_t lo = 0, hi = npdata;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (pdata[m].begin <= rva) lo = m + 1; else hi = m; }
    if (!lo) return NULL;
    const RF* f = &pdata[lo - 1]; return rva < f->end ? f : NULL;
}
static const unsigned char* unwind_info(unsigned rva) {
    size_t h = (rva * 2654435761u) & (uw_cap - 1);
    for (;;) {
        if (uw_cache_k[h] && uw_keys[h] == rva) return uw_cache_k[h];
        if (!uw_cache_k[h]) break;
        h = (h + 1) & (uw_cap - 1);
    }
    unsigned char* buf = malloc(560); size_t n = 560;
    while (n >= 16 && !rpm(img_base + rva, buf, n)) n /= 2;
    if (n < 16) { free(buf); return NULL; }
    uw_cache_k[h] = buf; uw_keys[h] = rva; return buf;
}
// Applies the unwind codes of f to regs (top: only those already executed at pc). 0 on failure.
static int apply_unwind(unsigned long long regs[16], const RF* f, unsigned long long pc, int top) {
    unsigned long long off_in = pc - img_base - f->begin;
    int first = 1;
    for (int chain = 0; chain < 16; ++chain) {
        const unsigned char* ui = unwind_info(f->unwind & ~1u);
        if (!ui || (ui[0] & 7) < 1 || (ui[0] & 7) > 2) return 0;
        unsigned prolog = ui[1], ncodes = ui[2], frame_reg = ui[3] & 15, frame_off = (ui[3] >> 4) * 16u;
        const unsigned short* codes = (const unsigned short*)(ui + 4);
        for (unsigned i = 0; i < ncodes;) {
            unsigned c = codes[i]; unsigned co = c & 255, op = (c >> 8) & 15, info = c >> 12; unsigned slots = 1;
            int apply = !(top && first) || off_in >= co;
            switch (op) {
            case 0: slots = 1; if (apply) { unsigned long long v; if (!rd64(regs[4], &v)) return 0; regs[info] = v; regs[4] += 8; } break;
            case 1: if (info == 0) { slots = 2; if (apply) regs[4] += (unsigned long long)codes[i + 1] * 8; }
                    else { slots = 3; if (apply) regs[4] += (unsigned long long)codes[i + 1] | ((unsigned long long)codes[i + 2] << 16); } break;
            case 2: if (apply) regs[4] += info * 8 + 8; break;
            case 3: if (apply) regs[4] = regs[frame_reg] - frame_off; break;
            case 4: slots = 2; if (apply) { unsigned long long v; if (!rd64(regs[4] + (unsigned long long)codes[i + 1] * 8, &v)) return 0; regs[info] = v; } break;
            case 5: slots = 3; if (apply) { unsigned long long v, o = (unsigned long long)codes[i + 1] | ((unsigned long long)codes[i + 2] << 16);
                    if (!rd64(regs[4] + o, &v)) return 0; regs[info] = v; } break;
            case 6: slots = 2; break;   // epilog (v2) / unused
            case 8: slots = 2; break;   // save xmm128
            case 9: slots = 3; break;   // save xmm128 far
            case 10: return 0;          // machine frame: not expected in user code
            default: return 0;
            }
            i += slots;
        }
        (void)prolog;
        if (ui[0] & 0x20) {   // chained
            unsigned n = (ncodes + 1) & ~1u;
            f = (const RF*)(ui + 4 + 2 * n); first = 0;
            static RF tmp[16]; tmp[chain] = *f; f = &tmp[chain];
            continue;
        }
        return 1;
    }
    return 0;
}
// pc is at the start/inside an epilogue (add rsp / lea rsp, pops, ret|tail jmp): unwind by executing it.
static int try_epilogue(unsigned long long regs[16], unsigned long long pc, const RF* f) {
    unsigned char b[48]; if (!rpm(pc, b, sizeof b)) return 0;
    unsigned long long rsp = regs[4], r[16]; memcpy(r, regs, sizeof r);
    int i = 0, pre = 0, pops = 0;
    if ((b[0] & 0xFE) == 0x48 && b[1] == 0x8D && (b[2] & 0x38) == 0x20) {
        int mod = b[2] >> 6, rm = b[2] & 7, base = rm | ((b[0] & 1) << 3);
        if (rm == 4) return 0;
        if (mod == 1) { rsp = r[base] + (long long)(signed char)b[3]; i = 4; }
        else if (mod == 2) { rsp = r[base] + (long long)*(int*)(b + 3); i = 7; }
        else return 0;
        pre = 1;
    } else if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xC4) { rsp += (long long)(signed char)b[3]; i = 4; pre = 1; }
    else if (b[0] == 0x48 && b[1] == 0x81 && b[2] == 0xC4) { rsp += (long long)*(int*)(b + 3); i = 7; pre = 1; }
    for (;;) {
        int reg = -1, len = 0;
        if (b[i] >= 0x58 && b[i] <= 0x5F) { reg = b[i] - 0x58; len = 1; }
        else if (b[i] == 0x41 && b[i + 1] >= 0x58 && b[i + 1] <= 0x5F) { reg = 8 + b[i + 1] - 0x58; len = 2; }
        else break;
        unsigned long long v; if (!rd64(rsp, &v)) return 0;
        r[reg] = v; rsp += 8; i += len; ++pops;
    }
    int ok = 0;
    if (b[i] == 0xC3 || (b[i] == 0xF3 && b[i + 1] == 0xC3)) ok = 1;
    else if (b[i] == 0xE9 || b[i] == 0xEB) {
        long long rel = b[i] == 0xE9 ? *(int*)(b + i + 1) : (signed char)b[i + 1];
        unsigned long long t = pc + i + (b[i] == 0xE9 ? 5 : 2) + rel;
        ok = !f || t < img_base + f->begin || t >= img_base + f->end;
    } else if (b[i] == 0xFF && b[i + 1] == 0x25) ok = 1;
    else if ((pre || pops) && ((b[i] == 0xFF && (b[i + 1] & 0xF8) == 0xE0) || (b[i] == 0x41 && b[i + 1] == 0xFF && (b[i + 2] & 0xF8) == 0xE0))) ok = 1;
    if (!ok) return 0;
    memcpy(regs, r, sizeof r); regs[4] = rsp; return 1;
}
#define MAX_DEPTH 700
// Fills frames[] (innermost first). Thread must be suspended. Returns the frame count (0 = none).
static int capture_stack(HANDLE th, DWORD tid, CONTEXT* c, unsigned long long* frames) {
    static NtQueryInformationThread_t nqit;
    if (!nqit) nqit = (NtQueryInformationThread_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    unsigned long long hi = 0;
    for (int i = 0; i < nstack_tops; ++i) if (stack_tops[i].tid == tid) hi = stack_tops[i].hi;
    if (!hi && nqit && nstack_tops < 512) {
        struct { LONG st; PVOID teb; ULONG_PTR a, b, c, d, e; } tbi; unsigned long long tib[3];
        if (nqit(th, 0, &tbi, sizeof tbi, NULL) == 0 && rpm((unsigned long long)tbi.teb, tib, sizeof tib)) hi = tib[1];
        stack_tops[nstack_tops].tid = tid; stack_tops[nstack_tops++].hi = hi;
    }
    unsigned long long regs[16] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rsp, c->Rbp, c->Rsi, c->Rdi,
                                   c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15};
    unsigned long long rip = c->Rip;
    frames[0] = rip;
    if (rip < img_base || rip >= img_base + img_size || !hi || regs[4] >= hi) return 1;
    unsigned long long want = hi - regs[4]; if (want > (1u << 20)) want = 1u << 20;
    slab_lo = regs[4]; slab_hi = slab_lo;
    while (want && !rpm(slab_lo, slab, (size_t)want)) want /= 2;   // stack top may be partly uncommitted
    if (want) slab_hi = slab_lo + want;
    int n = 1, top = 1;
    while (n < MAX_DEPTH) {
        const RF* f = find_rf(top ? rip : rip - 1);
        unsigned long long nrip, prsp = regs[4];
        if (f) {
            if (top && try_epilogue(regs, rip, f)) { if (!rd64(regs[4], &nrip)) break; regs[4] += 8; }
            else {
                if (!apply_unwind(regs, f, rip, top)) break;
                if (!rd64(regs[4], &nrip)) break; regs[4] += 8;
            }
        } else {
            if (!rd64(regs[4], &nrip)) break;
            regs[4] += 8;
        }
        if (nrip < img_base || nrip >= img_base + img_size || regs[4] <= prsp) break;
        frames[n++] = nrip; rip = nrip; top = 0;
    }
    return n;
}

// ---- Guest (MIPS) call chain. AOT guest calls are musttail jumps, so the HOST stack holds no guest nesting: the
// guest chain is walked on the guest stack in EE RAM instead. gpr[29] ($sp) and gpr[31] ($ra) as last written back
// identify the innermost caller X (ra is inside X in both the "just entered F" and the "F returned from a call"
// states); then each frame's saved ra is read at sp+raoff and sp advances by the frame size. Per-function
// frame/raoff come from SAMPLER_GUEST_TABLE (tsv: begin end frame raoff, written by inclusive.py --guest-table).
// The EeAotContext (gpr and ram pointers) is found once in a thread's stack by its field layout.
typedef struct { unsigned begin, end; int frame, raoff; } GFn;
static unsigned verify_addr; static unsigned char verify[16];
static GFn* gfn; static size_t ngfn; static unsigned long long g_gpr, g_ram; static DWORD g_ctx_tid;
static int load_guest_table(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return 0;
    size_t cap = 4096; gfn = malloc(cap * sizeof *gfn); char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned b, e; int fr, ro;
        if (line[0] == 'V') {   // V <addr> <16 hex bytes of the game's code there>: identifies EE RAM in the child
            char hex[40]; if (sscanf(line + 2, "%x %32s", &b, hex) == 2) {
                verify_addr = b; for (int i = 0; i < 16; ++i) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); verify[i] = (unsigned char)v; }
            }
            continue;
        }
        if (sscanf(line, "%x %x %d %d", &b, &e, &fr, &ro) != 4) continue;
        if (ngfn == cap) { cap *= 2; gfn = realloc(gfn, cap * sizeof *gfn); }
        gfn[ngfn++] = (GFn){b, e, fr, ro};
    }
    fclose(f); return ngfn > 0;
}
static const GFn* gfind(unsigned pc) {
    size_t lo = 0, hi = ngfn;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (gfn[m].begin <= pc) lo = m + 1; else hi = m; }
    return lo && pc < gfn[lo - 1].end ? &gfn[lo - 1] : NULL;
}
static int ctx_cands, ctx_tries;
static unsigned long long find_ram(void) {
    // EE RAM: a committed writable region of at least 32 MiB holding the game's code at verify_addr
    // (anywhere inside the region, in case the allocation is not at its start).
    MEMORY_BASIC_INFORMATION mbi; unsigned long long a = 0x10000;
    while (VirtualQueryEx(g_proc, (LPCVOID)a, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned long long b = (unsigned long long)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize >= 0x2000000 && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) {
            size_t chunk = 1u << 24;
            unsigned char* buf = malloc(chunk + 16);
            for (unsigned long long off = 0; off < mbi.RegionSize && buf; off += chunk) {
                size_t n = mbi.RegionSize - off < chunk + 16 ? (size_t)(mbi.RegionSize - off) : chunk + 16;
                if (!rpm(b + off, buf, n)) continue;
                for (size_t i = 0; i + 16 <= n && i < chunk; ++i)
                    if (buf[i] == verify[0] && !memcmp(buf + i, verify, 16)) {
                        unsigned long long ram = b + off + i - verify_addr;
                        if (ram >= b && ram + 0x2000000 <= b + mbi.RegionSize) { free(buf); return ram; }
                    }
            }
            free(buf);
        }
        a = b + mbi.RegionSize;
    }
    return 0;
}
static void find_ctx(DWORD tid) {
    if (getenv("SAMPLER_DEBUG") && ++ctx_tries % 200 == 1) fprintf(stderr, "[dbg] find_ctx try %d cands %d slab %llx..%llx\n", ctx_tries, ctx_cands, slab_lo, slab_hi);
    for (unsigned long long a = slab_lo; a + 64 <= slab_hi; a += 8) {
        unsigned long long q[8]; memcpy(q, slab + (a - slab_lo), 64);
        int ok = 1;
        for (int i = 0; i < 8 && ok; ++i) ok = q[i] > 0x10000 && q[i] < 0x800000000000ull;
        if (!ok || q[3] < q[0] || q[3] - q[0] > 0x10000) continue;
        ++ctx_cands;
        unsigned char g[512]; unsigned long long ra;
        if (!rpm(q[3], g, sizeof g)) continue;
        if (*(unsigned long long*)g != 0) continue;
        unsigned sp = *(unsigned*)(g + 29 * 16); ra = *(unsigned*)(g + 31 * 16);
        if (sp < 0x100000 || sp >= 0x2000000 || (sp & 15)) continue;
        if (!(ra == 0 || (ra >= 0x100000 && ra < 0x500000))) continue;
        g_gpr = q[3]; g_ctx_tid = tid;
        fprintf(stderr, "[sampler] guest context found on thread %lu: gpr %llx ram %llx\n", tid, g_gpr, g_ram);
        return;
    }
}
// Fills out[] with return addresses (innermost caller first); returns the count.
static int dbg_n;
static int guest_stack(unsigned long long* out, int max) {
    static unsigned char gs[0x10000]; unsigned char g[512];
    if (!rpm(g_gpr, g, sizeof g)) return 0;
    unsigned sp = *(unsigned*)(g + 29 * 16), ra = *(unsigned*)(g + 31 * 16);
    unsigned long long base = sp & 0x1FFFFFFu, span = 0x2000000u - base; if (span > sizeof gs) span = sizeof gs;
    if (!rpm(g_ram + base, gs, (size_t)span)) { span = 0x1000; if (!rpm(g_ram + base, gs, (size_t)span)) return 0; }
    unsigned sp0 = sp; int n = 0;
    while (n < max && ra >= 0x100000 && ra < 0x500000) {
        const GFn* x = gfind(ra - 4);
        out[n++] = ra;
        if (!x || x->raoff < 0 || x->frame <= 0) break;
        unsigned long long off = (unsigned long long)(sp - sp0) + (unsigned)x->raoff;
        if (sp < sp0 || off + 4 > span) break;
        unsigned nra = *(unsigned*)(gs + off);
        if (getenv("SAMPLER_DEBUG") && dbg_n < 60 && !(nra >= 0x100000 && nra < 0x500000)) {
            ++dbg_n; fprintf(stderr, "[dbg] BREAK depth %d sp %x ra %x fn %x frame %d raoff %d next %x | words:", n, sp, ra, x->begin, x->frame, x->raoff, nra);
            for (int k = 0; k < (x->frame < 160 ? x->frame / 4 : 40); ++k) fprintf(stderr, " %x", ((unsigned*)(gs + (sp - sp0)))[k]);
            fprintf(stderr, "\n");
        }
        ra = nra; sp += (unsigned)x->frame;
    }
    return n;
}
typedef struct { DWORD tid; unsigned count; unsigned n; size_t off; unsigned long long hash; } StkEnt;
static StkEnt* stks; static size_t nstks, stk_cap, stk_hcap; static size_t* stk_hash;
static unsigned long long* pool; static size_t pool_n, pool_cap;
static void add_stack(DWORD tid, const unsigned long long* fr, int n) {
    unsigned long long h = 1469598103934665603ull ^ tid;
    for (int i = 0; i < n; ++i) { h ^= fr[i]; h *= 1099511628211ull; h ^= h >> 29; }
    if (!stk_hash) { stk_hcap = 1u << 20; stk_hash = calloc(stk_hcap, sizeof *stk_hash); }
    if (nstks * 2 >= stk_hcap) return;   // table full: drop (cannot happen in a normal run)
    size_t k = h & (stk_hcap - 1);
    for (; stk_hash[k]; k = (k + 1) & (stk_hcap - 1)) {
        StkEnt* e = &stks[stk_hash[k] - 1];
        if (e->hash == h && e->tid == tid && e->n == (unsigned)n && !memcmp(pool + e->off, fr, (size_t)n * 8)) { e->count++; return; }
    }
    if (nstks == stk_cap) { stk_cap = stk_cap ? stk_cap * 2 : 1u << 16; stks = realloc(stks, stk_cap * sizeof *stks); }
    if (pool_n + n > pool_cap) { pool_cap = pool_cap ? pool_cap * 2 : 1u << 22; pool = realloc(pool, pool_cap * 8); }
    memcpy(pool + pool_n, fr, (size_t)n * 8);
    stks[nstks] = (StkEnt){tid, 1, (unsigned)n, pool_n, h}; pool_n += n; stk_hash[k] = ++nstks;
}
static unsigned long long exe_base_of(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 me = {sizeof me}; unsigned long long b = 0;
    if (snap != INVALID_HANDLE_VALUE) { if (Module32First(snap, &me)) b = (unsigned long long)me.modBaseAddr; CloseHandle(snap); }
    return b;
}

int main(int argc, char** argv) {
    if (argc < 6 || strcmp(argv[4], "--") != 0) { fprintf(stderr, "usage\n"); return 2; }
    const char* out = argv[1]; int interval_us = atoi(argv[2]); double delay = atof(argv[3]);
    char cmd[32768] = {0};
    for (int i = 5; i < argc; ++i) {
        strcat(cmd, "\""); strcat(cmd, argv[i]); strcat(cmd, "\" ");
    }
    table = calloc(cap, sizeof(Entry));
    DWORD sample_start = GetTickCount();
    STARTUPINFOA si = {sizeof si}; PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { fprintf(stderr, "spawn failed %lu\n", GetLastError()); return 3; }

    Sleep((DWORD)(delay * 1000));
    const char* start_file = getenv("SAMPLER_START_FILE");
    if (start_file) {
        while (GetFileAttributesA(start_file) == INVALID_FILE_ATTRIBUTES && WaitForSingleObject(pi.hProcess, 20) == WAIT_TIMEOUT) {}
        fprintf(stderr, "[sampler] start file seen, sampling\n");
    }
    sample_start = GetTickCount();
    int stack_every = getenv("SAMPLER_STACK_EVERY") ? atoi(getenv("SAMPLER_STACK_EVERY")) : 3;
    int have_img = 0;
    if (stack_every > 0) {
        g_proc = pi.hProcess;
        for (int tries = 0; tries < 50 && !have_img; ++tries) {
            unsigned long long b = exe_base_of(pi.dwProcessId);
            have_img = b && load_image(b);
            if (!have_img) Sleep(100);
        }
        fprintf(stderr, "[sampler] stacks %s (base %llx size %llx, %zu unwind entries)\n", have_img ? "on" : "unavailable", img_base, img_size, npdata);
    }
    static unsigned long long frames[MAX_DEPTH + 420];
    if (getenv("SAMPLER_GUEST_TABLE")) fprintf(stderr, "[sampler] guest table: %s\n", load_guest_table(getenv("SAMPLER_GUEST_TABLE")) ? "loaded" : "missing");
    unsigned long long total = 0, iterations = 0, stacked = 0;
    HANDLE threads[256]; DWORD tids[256]; int nthreads = 0; DWORD last_scan = 0;
    while (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
        DWORD now = GetTickCount();
        if (nthreads == 0 || now - last_scan > 2000) {
            for (int i = 0; i < nthreads; ++i) CloseHandle(threads[i]);
            nthreads = 0; last_scan = now;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = {sizeof te};
            if (Thread32First(snap, &te)) do {
                if (te.th32OwnerProcessID == pi.dwProcessId && nthreads < 256) {
                    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                    if (t) { threads[nthreads] = t; tids[nthreads++] = te.th32ThreadID; }
                }
            } while (Thread32Next(snap, &te));
            CloseHandle(snap);
        }
        if (++iterations % 50 == 1) for (int i = 0; i < nthreads; ++i) note_cpu(tids[i], threads[i]);
        for (int i = 0; i < nthreads; ++i) {
            if (SuspendThread(threads[i]) == (DWORD)-1) continue;
            CONTEXT c; c.ContextFlags = have_img ? (CONTEXT_CONTROL | CONTEXT_INTEGER) : CONTEXT_CONTROL;
            if (GetThreadContext(threads[i], &c)) {
                add(tids[i], c.Rip); total++;
                if (have_img && c.Rip >= img_base && c.Rip < img_base + img_size && iterations % stack_every == 0) {
                    int n = capture_stack(threads[i], tids[i], &c, frames);
                    if (n > 0) {
                        if (ngfn && !g_gpr && n > 1) {
                            static unsigned tries;
                            if (!g_ram && tries % 400 == 0) g_ram = find_ram();
                            if (g_ram && tries % 10 == 0) find_ctx(tids[i]);
                            ++tries;
                        }
                        if (g_gpr && tids[i] == g_ctx_tid) {
                            frames[n++] = 0;
                            n += guest_stack(frames + n, 400);
                        }
                        add_stack(tids[i], frames, n); stacked++;
                    }
                }
            }
            ResumeThread(threads[i]);
        }
        if (interval_us >= 1000) Sleep((DWORD)(interval_us / 1000)); else SwitchToThread();
    }
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    FILE* f = fopen(out, "w");
    fprintf(f, "# samples=%llu exit=%lu wall_ms=%lu\n", total, code, (unsigned long)(GetTickCount() - sample_start));
    for (int i = 0; i < ncpus; ++i) fprintf(f, "# cpu %lu %.1f\n", cpus[i].tid, (cpus[i].last - cpus[i].start) / 1e4);
    for (size_t i = 0; i < cap; ++i) if (table[i].count) fprintf(f, "%lu\t%llx\t%u\n", table[i].tid, table[i].rip, table[i].count);
    fclose(f);
    if (have_img) {
        char path[4096]; snprintf(path, sizeof path, "%s.stk", out);
        f = fopen(path, "w");
        fprintf(f, "# base=%llx size=%llx stacks=%zu samples=%llu every=%d\n", img_base, img_size, nstks, stacked, stack_every);
        for (size_t i = 0; i < nstks; ++i) {
            fprintf(f, "%lu\t%u\t", stks[i].tid, stks[i].count);
            int sep = 0;   // a 0 entry separates the host frames from the guest return addresses
            for (unsigned j = 0; j < stks[i].n; ++j) {
                unsigned long long v = pool[stks[i].off + j];
                if (v == 0) { fputc('\t', f); sep = 2; continue; }
                fprintf(f, sep == 1 ? ",%llx" : "%llx", v); sep = 1;
            }
            fputc('\n', f);
        }
        fclose(f);
    }
    return (int)code;
}
