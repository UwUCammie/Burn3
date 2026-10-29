// nullgs.exe <pipe>: stands in for parallel-gs-stream.exe. Opens the game's GS pipe (retrying while the
// game creates it), reads and discards everything until the game closes it. Prints bytes/s every 5 s.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
// NULLGS_HASH=<file>: parse the raw GS stream (include/takedown/platform/parallel_gs_stream.hpp; framing:
// kind 0 = Transfer [kind,path,u32 n]+n bytes; kind 3 = 8195 B
// (PrivRegisters + Vsync); kinds 4,5,7 = [kind,u32 n]+n bytes; kind 6 = 11 B) and write, per vblank:
//   F <n> <bytes> <full> <path0> <path1> <path2> <hdr> <p2bytes> <largest-path2-transfer>
// full = FNV-1a 64 of every byte since the previous vblank packet (inclusive); pathN = hash of the payload bytes of
// that path's Transfers (in order); hdr = hash of everything else (framing, priv registers, other record kinds).
// Last line: "T <frames> <bytes> <whole-stream-hash>" plus "R <from> <to> <frames> <bytes> <hash>" over the vblank
// range NULLGS_HASH_FROM..NULLGS_HASH_TO (frame indices, inclusive; default everything). NULLGS_HASH_MASK_P2=<min
// bytes>: also "M" hash column = full hash with path-2 Transfers of at least that many payload bytes left out.
#define FNV0 1469598103934665603ULL
#define FNVP 1099511628211ULL
static struct {
    FILE* out; uint64_t from, to, mask_min;
    uint64_t full, ph[3], hdr, mh, fb, p2b, p2max, nframe, tb, th, rh, rb, rframes, maskcur;
    int stage, kind, hlen, bad, inrange_any; uint64_t need, payload; unsigned char hdrb[8]; int path;
} H;
static inline uint64_t fnv(uint64_t h, const unsigned char* p, size_t n) { for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= FNVP; } return h; }
static void h_feed(const unsigned char* p, size_t n, int which) { // which: -1 header bytes, 0..2 path payload
    H.full = fnv(H.full, p, n);
    if (which >= 0) H.ph[which] = fnv(H.ph[which], p, n); else H.hdr = fnv(H.hdr, p, n);
    if (!(which == 2 && H.maskcur)) H.mh = fnv(H.mh, p, n);
    H.fb += n; H.tb += n; H.th = fnv(H.th, p, n);
    if (H.nframe >= H.from && H.nframe <= H.to) { H.rh = fnv(H.rh, p, n); H.rb += n; }
}
static void h_run(HANDLE h) {
    static unsigned char buf[1 << 20]; DWORD n;
    H.full = H.hdr = H.mh = H.th = H.rh = FNV0; H.ph[0] = H.ph[1] = H.ph[2] = FNV0;
    while (ReadFile(h, buf, sizeof buf, &n, NULL) && n > 0) {
        size_t pos = 0;
        while (pos < n) {
            if (H.bad) { h_feed(buf + pos, n - pos, -1); pos = n; break; }
            if (H.stage == 0) {
                H.kind = buf[pos]; H.hlen = 0; H.stage = 1; H.path = -1; H.maskcur = 0;
                switch (H.kind) { case 0: H.need = 6; break; case 3: H.need = 8195; break; case 4: case 5: case 7: H.need = 5; break; case 6: H.need = 11; break;
                    default: H.bad = 1; fprintf(H.out, "BAD kind %d at %llu\n", H.kind, (unsigned long long)H.tb); continue; }
            }
            if (H.stage == 1) {
                size_t take = n - pos < H.need - (uint64_t)H.hlen ? n - pos : (size_t)(H.need - H.hlen);
                size_t keep = 8 - H.hlen < take ? 8 - H.hlen : take; if (H.hlen < 8) memcpy(H.hdrb + H.hlen, buf + pos, keep);
                h_feed(buf + pos, take, -1); pos += take; H.hlen += (int)take;
                if ((uint64_t)H.hlen == H.need) {
                    if (H.kind == 0) { H.payload = H.hdrb[2] | (H.hdrb[3] << 8) | (H.hdrb[4] << 16) | ((uint64_t)H.hdrb[5] << 24); H.path = H.hdrb[1] < 3 ? H.hdrb[1] : -1;
                        if (H.path == 2) { H.p2b += H.payload; if (H.payload > H.p2max) H.p2max = H.payload; H.maskcur = H.mask_min && H.payload >= H.mask_min; } }
                    else if (H.kind == 4 || H.kind == 5 || H.kind == 7) H.payload = H.hdrb[1] | (H.hdrb[2] << 8) | (H.hdrb[3] << 16) | ((uint64_t)H.hdrb[4] << 24);
                    else H.payload = 0;
                    if (H.kind == 3) {
                        fprintf(H.out, "F %llu %llu %016llx %016llx %016llx %016llx %016llx %016llx %llu %llu\n", (unsigned long long)H.nframe, (unsigned long long)H.fb,
                                (unsigned long long)H.full, (unsigned long long)H.ph[0], (unsigned long long)H.ph[1], (unsigned long long)H.ph[2],
                                (unsigned long long)H.hdr, (unsigned long long)H.mh, (unsigned long long)H.p2b, (unsigned long long)H.p2max);
                        if (H.nframe >= H.from && H.nframe <= H.to) ++H.rframes;
                        ++H.nframe; H.full = H.hdr = H.mh = FNV0; H.ph[0] = H.ph[1] = H.ph[2] = FNV0; H.fb = H.p2b = H.p2max = 0; H.stage = 0;
                    } else if (H.payload) { H.stage = 2; H.need = H.payload; } else H.stage = 0;
                }
            } else {
                size_t take = n - pos < H.need ? n - pos : (size_t)H.need;
                h_feed(buf + pos, take, H.path); pos += take; H.need -= take; if (H.need == 0) H.stage = 0;
            }
        }
    }
    fprintf(H.out, "T %llu %llu %016llx tail %llu\n", (unsigned long long)H.nframe, (unsigned long long)H.tb, (unsigned long long)H.th, (unsigned long long)H.fb);
    fprintf(H.out, "R %llu %llu %llu %llu %016llx\n", (unsigned long long)H.from, (unsigned long long)(H.to == ~0ULL ? H.nframe - 1 : H.to), (unsigned long long)H.rframes, (unsigned long long)H.rb, (unsigned long long)H.rh);
    fclose(H.out);
}
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 1200 && h == INVALID_HANDLE_VALUE; ++i) {
        h = CreateFileA(argv[1], GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) Sleep(100);
    }
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "nullgs: could not open %s\n", argv[1]); return 1; }
    if (getenv("NULLGS_HASH")) {
        H.out = fopen(getenv("NULLGS_HASH"), "w");
        if (!H.out) return 3;
        H.from = getenv("NULLGS_HASH_FROM") ? strtoull(getenv("NULLGS_HASH_FROM"), 0, 10) : 0;
        H.to = getenv("NULLGS_HASH_TO") ? strtoull(getenv("NULLGS_HASH_TO"), 0, 10) : ~0ULL;
        H.mask_min = getenv("NULLGS_HASH_MASK_P2") ? strtoull(getenv("NULLGS_HASH_MASK_P2"), 0, 10) : 0;
        h_run(h);
        return 0;
    }
    // NULLGS_DUMP=<file>: also write every received byte to that file (a recorded stream for offline replay).
    FILE* dump = getenv("NULLGS_DUMP") ? fopen(getenv("NULLGS_DUMP"), "wb") : NULL;
    static char buf[1 << 20];
    unsigned long long total = 0, last = 0; DWORD t0 = GetTickCount(), tl = t0, n;
    while (ReadFile(h, buf, sizeof buf, &n, NULL) && n > 0) {
        total += n;
        if (dump) fwrite(buf, 1, n, dump);
        DWORD now = GetTickCount();
        if (now - tl >= 5000) { fprintf(stderr, "nullgs: %.1f MB/s\n", (total - last) / 1e6 / ((now - tl) / 1000.0)); last = total; tl = now; }
    }
    if (dump) fclose(dump);
    fprintf(stderr, "nullgs: closed after %llu bytes\n", total);
    return 0;
}
