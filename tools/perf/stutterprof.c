// Stutter profiler for players' own races (built with -mwindows: no console window): stutterprof.exe <pid> <out-dir> [interval_us]
// Attaches to the running game (the game starts it when config/profiler.txt exists, see main.cpp), samples the
// call stack of every thread that used CPU in the last 50 ms about once per interval (default 4000 us), and writes
// until the game exits:
//   <out-dir>/samples.bin   one record per sample: u64 time (ns, the clock of std::chrono::steady_clock in the game),
//                           u32 thread id, u32 stack id
//   <out-dir>/stacks.txt    stack id \t frames (hex, innermost first)
//   <out-dir>/threads.txt   thread id \t CPU ms while sampled \t samples
//   <out-dir>/sampler.txt   image base and size, totals
// tools/perf/stutter_report.py lines the samples up with the game's frames.bin. The host stack walker is the one
// in sampler.c.
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define MAX_DEPTH 64
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
    if (!hi || regs[4] >= hi) return 1;
    int outside = rip < img_base || rip >= img_base + img_size;
    unsigned long long want = hi - regs[4]; if (want > (1u << 16)) want = 1u << 16;   // 64 frames fit
    slab_lo = regs[4]; slab_hi = slab_lo;
    while (want && !rpm(slab_lo, slab, (size_t)want)) want /= 2;   // stack top may be partly uncommitted
    if (want) slab_hi = slab_lo + want;
    int n = 1, top = 1;
    if (outside) {
        // Inside a system DLL (memcpy, a wait, a Wine call): the exe's unwind tables do not cover it, so the first
        // stack slot that holds a return address into the exe (the byte before it ends a call instruction) is taken
        // as the caller, and the walk goes on from there.
        int found = 0;
        for (unsigned long long a = slab_lo; a + 8 <= slab_hi && a < slab_lo + 0x4000; a += 8) {
            unsigned long long v = *(unsigned long long*)(slab + (a - slab_lo));
            if (v < img_base + 8 || v >= img_base + img_size || !find_rf(v - 1)) continue;
            unsigned char b[8];
            if (!rpm(v - 8, b, sizeof b)) continue;
            if (b[3] == 0xE8 || (b[2] == 0xFF && b[3] == 0x15) || (b[6] == 0xFF && (b[7] & 0xF8) == 0xD0)) {
                frames[n++] = v; rip = v; regs[4] = a + 8; top = 0; found = 1; break;
            }
        }
        if (!found) return 1;
    }
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


// The game's clock: libstdc++'s steady_clock on MinGW is clock_gettime(CLOCK_MONOTONIC) from winpthreads, which is
// QueryPerformanceCounter split into seconds and rounded nanoseconds exactly like this.
static unsigned long long now_ns(void) {
    static LARGE_INTEGER freq; LARGE_INTEGER pc;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&pc);
    unsigned long long sec = (unsigned long long)(pc.QuadPart / freq.QuadPart);
    unsigned long long nsec = (unsigned long long)(((pc.QuadPart % freq.QuadPart) * 1000000000LL + (freq.QuadPart >> 1)) / freq.QuadPart);
    return sec * 1000000000ULL + nsec;
}

// Distinct stacks, deduplicated by hash.
typedef struct { unsigned long long hash; unsigned n; size_t off; } Stack;
static Stack* stacks; static size_t nstacks, stacks_cap, hcap = 1u << 21; static unsigned* htab;
static unsigned long long* pool; static size_t pool_n, pool_cap;
static unsigned stack_id(const unsigned long long* fr, int n) {
    unsigned long long h = 1469598103934665603ull;
    for (int i = 0; i < n; ++i) { h ^= fr[i]; h *= 1099511628211ull; h ^= h >> 29; }
    if (!htab) htab = calloc(hcap, sizeof *htab);
    size_t k = h & (hcap - 1);
    for (; htab[k]; k = (k + 1) & (hcap - 1)) {
        Stack* s = &stacks[htab[k] - 1];
        if (s->hash == h && s->n == (unsigned)n && !memcmp(pool + s->off, fr, (size_t)n * 8)) return htab[k] - 1;
    }
    if (nstacks * 2 >= hcap) return 0;
    if (nstacks == stacks_cap) { stacks_cap = stacks_cap ? stacks_cap * 2 : 1u << 16; stacks = realloc(stacks, stacks_cap * sizeof *stacks); }
    if (pool_n + n > pool_cap) { pool_cap = pool_cap ? pool_cap * 2 : 1u << 22; pool = realloc(pool, pool_cap * 8); }
    memcpy(pool + pool_n, fr, (size_t)n * 8);
    stacks[nstacks] = (Stack){h, (unsigned)n, pool_n}; pool_n += n; htab[k] = (unsigned)++nstacks;
    return (unsigned)(nstacks - 1);
}

typedef struct { DWORD tid; HANDLE h; unsigned long long cpu_start, cpu_last; unsigned long long samples; int busy; } Thread;
static Thread thr[512]; static int nthr;
static unsigned long long thread_cpu(HANDLE t) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(t, &c, &e, &k, &u)) return 0;
    return (((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime);
}
static void scan_threads(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = {sizeof te};
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != pid) continue;
        int known = 0;
        for (int i = 0; i < nthr; ++i) if (thr[i].tid == te.th32ThreadID) { known = 1; break; }
        if (known || nthr == 512) continue;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (t) { unsigned long long c = thread_cpu(t); thr[nthr++] = (Thread){te.th32ThreadID, t, c, c, 0, 1}; }
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
}
static void write_threads(const char* dir) {
    char path[4096]; snprintf(path, sizeof path, "%s/threads.txt", dir);
    FILE* f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < nthr; ++i) fprintf(f, "%lu\t%.1f\t%llu\n", thr[i].tid, (thr[i].cpu_last - thr[i].cpu_start) / 1e4, thr[i].samples);
    fclose(f);
}
static unsigned long long exe_base_of(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32 me = {sizeof me}; unsigned long long b = 0;
    if (snap != INVALID_HANDLE_VALUE) { if (Module32First(snap, &me)) b = (unsigned long long)me.modBaseAddr; CloseHandle(snap); }
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: stutterprof.exe <pid> <out-dir> [interval_us]\n"); return 2; }
    DWORD pid = (DWORD)strtoul(argv[1], NULL, 10); const char* dir = argv[2];
    int interval_us = argc > 3 ? atoi(argv[3]) : 4000;
    g_proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
    if (!g_proc) { fprintf(stderr, "cannot open process %lu (%lu)\n", pid, GetLastError()); return 3; }
    char path[4096];
    snprintf(path, sizeof path, "%s/samples.bin", dir);
    FILE* out = fopen(path, "wb"); if (!out) return 4;
    setvbuf(out, NULL, _IOFBF, 1 << 20);
    int have_img = 0;
    for (int tries = 0; tries < 100 && !have_img; ++tries) {
        unsigned long long b = exe_base_of(pid);
        have_img = b && load_image(b);
        if (!have_img) Sleep(100);
    }
    snprintf(path, sizeof path, "%s/sampler.txt", dir);
    FILE* f = fopen(path, "w");
    if (f) { fprintf(f, "base=%llx size=%llx interval_us=%d\n", img_base, img_size, interval_us); fclose(f); }
    static unsigned long long frames[MAX_DEPTH + 8];
    unsigned long long total = 0, last_scan = 0, last_busy = 0, last_flush = 0;
    while (WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT) {
        unsigned long long t = now_ns();
        if (t - last_scan > 2000000000ull) { scan_threads(pid); last_scan = t; }
        if (t - last_busy > 50000000ull) {   // a thread is sampled while it used CPU in the last 50 ms
            for (int i = 0; i < nthr; ++i) {
                unsigned long long c = thread_cpu(thr[i].h);
                thr[i].busy = c != thr[i].cpu_last; thr[i].cpu_last = c;
            }
            last_busy = t;
        }
        for (int i = 0; i < nthr; ++i) {
            if (!thr[i].busy) continue;
            if (SuspendThread(thr[i].h) == (DWORD)-1) { thr[i].busy = 0; continue; }
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            int n = 0;
            if (GetThreadContext(thr[i].h, &c)) n = have_img ? capture_stack(thr[i].h, thr[i].tid, &c, frames) : (frames[0] = c.Rip, 1);
            unsigned long long when = now_ns();
            ResumeThread(thr[i].h);
            if (n <= 0) continue;
            struct { unsigned long long ns; unsigned tid, stack; } rec = {when, thr[i].tid, stack_id(frames, n)};
            fwrite(&rec, sizeof rec, 1, out); thr[i].samples++; total++;
        }
        if (t - last_flush > 1000000000ull) { fflush(out); write_threads(dir); last_flush = t; }
        if (interval_us >= 1000) Sleep((DWORD)(interval_us / 1000)); else SwitchToThread();
    }
    fclose(out);
    snprintf(path, sizeof path, "%s/stacks.txt", dir);
    f = fopen(path, "w");
    for (size_t i = 0; i < nstacks; ++i) {
        fprintf(f, "%zu\t", i);
        for (unsigned j = 0; j < stacks[i].n; ++j) fprintf(f, j ? ",%llx" : "%llx", pool[stacks[i].off + j]);
        fputc('\n', f);
    }
    fclose(f);
    write_threads(dir);
    snprintf(path, sizeof path, "%s/sampler.txt", dir);
    f = fopen(path, "w");
    fprintf(f, "base=%llx size=%llx samples=%llu stacks=%zu interval_us=%d\n", img_base, img_size, total, nstacks, interval_us);
    fclose(f);
    return 0;
}
