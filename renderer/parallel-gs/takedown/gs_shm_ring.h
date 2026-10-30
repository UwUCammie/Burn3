#pragma once

// Shared-memory carrier for the GS command stream (game -> renderer). Single producer, single consumer.
// Header-only, C99/C++14, kernel32 only (CreateFileMapping/MapViewOfFile/CreateEvent/WaitForMultipleObjects):
// the same code runs on native Windows and under Wine. The stream bytes are exactly those the named pipe
// would carry; only the carrier changes. The named pipe stays connected as the handshake channel (see
// gs_shm_hello_*) and for end-of-stream, so an older peer simply never sees the ring.
//
// Layout: 4096-byte header, then the ring (power of two). Producer-written and consumer-written words live
// on separate cache lines. Blocking: spin for spin_us, then publish "sleeping" and wait on an auto-reset
// event; the peer signals the event only when the sleeping flag is set, so a busy stream costs no kernel
// calls at all. Every wait also waits on the peer's process handle, so a crashed peer never hangs us, and
// has a 500 ms timeout as a belt-and-braces re-check.
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#if defined(__SSE2__)
#include <emmintrin.h>
#define GS_SHM_PAUSE() _mm_pause()
#else
#define GS_SHM_PAUSE() ((void)0)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GS_SHM_MAGIC 0x4d485347u /* "GSHM" */
#define GS_SHM_VERSION 1u
#define GS_SHM_HEADER_BYTES 4096u
#define GS_SHM_ALIGN64 __attribute__((aligned(64)))

typedef struct GsShmHeader {
    uint32_t magic, version, ring_bytes, reserved;
    GS_SHM_ALIGN64 volatile uint64_t head;          /* bytes ever published by the producer */
    GS_SHM_ALIGN64 volatile uint64_t tail;          /* bytes ever released by the consumer */
    GS_SHM_ALIGN64 volatile uint32_t consumer_sleeping;
    volatile uint32_t producer_closed;              /* clean end of stream */
    GS_SHM_ALIGN64 volatile uint32_t producer_sleeping;
    volatile uint32_t consumer_closed;              /* consumer went away on purpose */
} GsShmHeader;

typedef struct GsShm {
    GsShmHeader* h;
    uint8_t* ring;
    HANDLE map, ev_data, ev_space, peer;
    uint64_t mask, local_head, local_tail, published_tail;          /* producer: local_head + cached tail; consumer: local_tail + cached head */
    uint32_t ring_bytes, spin_us;
    int producer;
    int peer_dead;
    /* statistics */
    uint64_t waits, wakes;
} GsShm;

static inline double gs_shm_now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}

static inline void gs_shm_names(const char* base, char* m, char* d, char* s, size_t cap) {
    snprintf(m, cap, "Local\\takedown-gs-%s-m", base);
    snprintf(d, cap, "Local\\takedown-gs-%s-d", base);
    snprintf(s, cap, "Local\\takedown-gs-%s-s", base);
}

static inline void gs_shm_release_handles(GsShm* s) {
    if (s->h) UnmapViewOfFile(s->h);
    if (s->map) CloseHandle(s->map);
    if (s->ev_data) CloseHandle(s->ev_data);
    if (s->ev_space) CloseHandle(s->ev_space);
    memset(s, 0, sizeof *s);
}

/* Producer: creates the objects. `peer` is a SYNCHRONIZE handle of the consumer process (owned by the ring
 * afterwards; may be NULL). Returns 0 on success (including when the name already exists: that is a failure
 * -1, so a stale object never aliases a new session). */
static inline int gs_shm_producer_create(GsShm* s, const char* base, uint32_t ring_bytes, uint32_t spin_us, HANDLE peer) {
    char m[160], d[160], e[160];
    memset(s, 0, sizeof *s);
    if (ring_bytes < 65536u || (ring_bytes & (ring_bytes - 1u))) return -1;
    gs_shm_names(base, m, d, e, sizeof m);
    s->map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, GS_SHM_HEADER_BYTES + ring_bytes, m);
    if (!s->map) return -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { gs_shm_release_handles(s); return -1; }
    s->ev_data = CreateEventA(NULL, FALSE, FALSE, d);
    s->ev_space = CreateEventA(NULL, FALSE, FALSE, e);
    s->h = (GsShmHeader*)MapViewOfFile(s->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!s->ev_data || !s->ev_space || !s->h) { gs_shm_release_handles(s); return -1; }
    s->h->magic = GS_SHM_MAGIC;
    s->h->version = GS_SHM_VERSION;
    s->h->ring_bytes = ring_bytes;
    s->ring = (uint8_t*)s->h + GS_SHM_HEADER_BYTES;
    s->ring_bytes = ring_bytes;
    s->mask = ring_bytes - 1u;
    s->spin_us = spin_us;
    s->peer = peer;
    s->producer = 1;
    return 0;
}

/* Consumer: opens the objects the producer made. */
static inline int gs_shm_consumer_open(GsShm* s, const char* base, uint32_t spin_us, HANDLE peer) {
    char m[160], d[160], e[160];
    memset(s, 0, sizeof *s);
    gs_shm_names(base, m, d, e, sizeof m);
    s->map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, m);
    if (!s->map) return -1;
    s->ev_data = OpenEventA(EVENT_ALL_ACCESS, FALSE, d);
    s->ev_space = OpenEventA(EVENT_ALL_ACCESS, FALSE, e);
    s->h = (GsShmHeader*)MapViewOfFile(s->map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!s->ev_data || !s->ev_space || !s->h || s->h->magic != GS_SHM_MAGIC || s->h->version != GS_SHM_VERSION ||
        s->h->ring_bytes < 65536u || (s->h->ring_bytes & (s->h->ring_bytes - 1u))) {
        gs_shm_release_handles(s);
        return -1;
    }
    s->ring_bytes = s->h->ring_bytes;
    s->ring = (uint8_t*)s->h + GS_SHM_HEADER_BYTES;
    s->mask = s->ring_bytes - 1u;
    s->spin_us = spin_us;
    s->peer = peer;
    s->producer = 0;
    return 0;
}

static inline void gs_shm_wait(GsShm* s, HANDLE ev) {
    HANDLE hs[2] = { ev, s->peer };
    ++s->waits;
    const DWORD r = WaitForMultipleObjects(s->peer ? 2u : 1u, hs, FALSE, 500u);
    if (r == WAIT_OBJECT_0 + 1u) s->peer_dead = 1;
}

/* Producer: appends n bytes, blocking while the ring is full. 0 = ok, -1 = the consumer is gone. */
static inline int gs_shm_write(GsShm* s, const void* data, size_t n) {
    GsShmHeader* const h = s->h;
    const uint8_t* d = (const uint8_t*)data;
    uint64_t head = s->local_head;
    while (n) {
        uint64_t freeb = s->ring_bytes - (head - s->local_tail);
        if (!freeb) {
            double t0 = 0;
            for (;;) {
                s->local_tail = __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE);
                if (head - s->local_tail < s->ring_bytes) break;
                if (s->peer_dead || __atomic_load_n(&h->consumer_closed, __ATOMIC_RELAXED)) return -1;
                if (s->spin_us) {
                    if (t0 == 0) t0 = gs_shm_now_us();
                    if (gs_shm_now_us() - t0 < s->spin_us) { GS_SHM_PAUSE(); continue; }
                }
                __atomic_store_n(&h->producer_sleeping, 1u, __ATOMIC_SEQ_CST);
                s->local_tail = __atomic_load_n(&h->tail, __ATOMIC_SEQ_CST);
                if (head - s->local_tail < s->ring_bytes) { __atomic_store_n(&h->producer_sleeping, 0u, __ATOMIC_RELAXED); break; }
                gs_shm_wait(s, s->ev_space);
                __atomic_store_n(&h->producer_sleeping, 0u, __ATOMIC_RELAXED);
                t0 = 0;
            }
            freeb = s->ring_bytes - (head - s->local_tail);
        }
        const size_t m = n < freeb ? n : (size_t)freeb;
        const size_t off = (size_t)(head & s->mask);
        const size_t a = s->ring_bytes - off < m ? (size_t)(s->ring_bytes - off) : m;
        memcpy(s->ring + off, d, a);
        if (m > a) memcpy(s->ring, d + a, m - a);
        head += m; d += m; n -= m;
        s->local_head = head;
        __atomic_store_n(&h->head, head, __ATOMIC_RELEASE);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&h->consumer_sleeping, __ATOMIC_RELAXED)) { ++s->wakes; SetEvent(s->ev_data); }
    }
    return 0;
}

/* Producer: end of stream. The consumer drains what is in the ring, then sees EOF. */
static inline void gs_shm_producer_close(GsShm* s) {
    if (!s->h) return;
    __atomic_store_n(&s->h->producer_closed, 1u, __ATOMIC_SEQ_CST);
    SetEvent(s->ev_data);
}

/* Consumer: makes everything consumed so far visible to the producer. Called lazily (see gs_shm_release) and
 * always before the consumer blocks, so the producer can never wait for space the consumer is holding. */
static inline void gs_shm_publish_tail(GsShm* s) {
    if (s->published_tail == s->local_tail) return;
    s->published_tail = s->local_tail;
    __atomic_store_n(&s->h->tail, s->local_tail, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s->h->producer_sleeping, __ATOMIC_RELAXED)) { ++s->wakes; SetEvent(s->ev_space); }
}

/* Consumer: blocks until bytes are available; returns how many are contiguous at *out (0 = end of stream:
 * the producer closed, or died, and everything it wrote has been consumed). */
static inline size_t gs_shm_acquire(GsShm* s, const uint8_t** out) {
    GsShmHeader* const h = s->h;
    for (;;) {
        const uint64_t head = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
        if (head != s->local_tail) {
            const size_t off = (size_t)(s->local_tail & s->mask);
            const uint64_t avail = head - s->local_tail;
            const size_t a = s->ring_bytes - off < avail ? (size_t)(s->ring_bytes - off) : (size_t)avail;
            *out = s->ring + off;
            return a;
        }
        if (s->peer_dead || __atomic_load_n(&h->producer_closed, __ATOMIC_ACQUIRE)) {
            if (__atomic_load_n(&h->head, __ATOMIC_ACQUIRE) == s->local_tail) return 0;
            continue;
        }
        gs_shm_publish_tail(s);
        if (s->spin_us) {
            const double t0 = gs_shm_now_us();
            int hit = 0;
            while (gs_shm_now_us() - t0 < s->spin_us) {
                if (__atomic_load_n(&h->head, __ATOMIC_ACQUIRE) != s->local_tail) { hit = 1; break; }
                GS_SHM_PAUSE();
            }
            if (hit) continue;
        }
        __atomic_store_n(&h->consumer_sleeping, 1u, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&h->head, __ATOMIC_SEQ_CST) != s->local_tail || __atomic_load_n(&h->producer_closed, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(&h->consumer_sleeping, 0u, __ATOMIC_RELAXED);
            continue;
        }
        gs_shm_wait(s, s->ev_data);
        __atomic_store_n(&h->consumer_sleeping, 0u, __ATOMIC_RELAXED);
    }
}

/* Consumer: consumes n bytes (<= what acquire returned). The producer sees the space at the latest when the
 * consumer next blocks, and after every ring_bytes/8 consumed, so tiny reads do not cost an atomic fence each. */
static inline void gs_shm_release(GsShm* s, size_t n) {
    s->local_tail += n;
    if (s->local_tail - s->published_tail >= (s->ring_bytes >> 3)) gs_shm_publish_tail(s);
}

/* Consumer: copies up to cap bytes (at least one unless end of stream); returns the count, 0 = end. */
static inline size_t gs_shm_read_some(GsShm* s, void* dst, size_t cap) {
    const uint8_t* p;
    size_t n = gs_shm_acquire(s, &p);
    if (!n) return 0;
    if (n > cap) n = cap;
    memcpy(dst, p, n);
    gs_shm_release(s, n);
    return n;
}

/* Consumer: exact read; returns fewer than n only at end of stream. */
static inline size_t gs_shm_read(GsShm* s, void* dst, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    size_t done = 0;
    while (done < n) {
        const size_t got = gs_shm_read_some(s, d + done, n - done);
        if (!got) break;
        done += got;
    }
    return done;
}

static inline void gs_shm_consumer_close(GsShm* s) {
    if (!s->h) return;
    gs_shm_publish_tail(s);
    __atomic_store_n(&s->h->consumer_closed, 1u, __ATOMIC_SEQ_CST);
    SetEvent(s->ev_space);
}

static inline void gs_shm_close(GsShm* s) {
    if (s->peer) CloseHandle(s->peer);
    s->peer = NULL;
    gs_shm_release_handles(s);
}

/* ---- Handshake. The renderer (consumer) announces support by creating the event
 * "Local\takedown-gs-cap-<leaf>" before it opens the game's pipe (<leaf> = the pipe name after \\.\pipe\).
 * After the pipe connects, a game that wants the ring and finds that event creates the objects and writes
 * gs_shm_hello (16 bytes, first byte 0xFE, never a valid record kind) as the first and only pipe payload.
 * A renderer that sees any other first byte keeps reading the pipe as before. */
#define GS_SHM_HELLO_BYTES 16u
static inline void gs_shm_hello_encode(uint8_t* b, uint32_t ring_bytes) {
    memset(b, 0, GS_SHM_HELLO_BYTES);
    b[0] = 0xFE; b[1] = 'G'; b[2] = 'S'; b[3] = 'H'; b[4] = 'M'; b[5] = (uint8_t)GS_SHM_VERSION;
    memcpy(b + 8, &ring_bytes, 4);
}
static inline int gs_shm_hello_decode(const uint8_t* b, uint32_t* ring_bytes) {
    if (b[0] != 0xFE || b[1] != 'G' || b[2] != 'S' || b[3] != 'H' || b[4] != 'M' || b[5] != GS_SHM_VERSION) return 0;
    memcpy(ring_bytes, b + 8, 4);
    return 1;
}
/* base = FNV-1a 64 of the pipe name's leaf as UTF-16 units (both sides widen the same UTF-8 path). */
static inline void gs_shm_cap_name(const wchar_t* pipe_path, char* base, size_t cap) {
    const wchar_t* leaf = pipe_path;
    for (const wchar_t* p = pipe_path; *p; ++p) if (*p == L'\\') leaf = p + 1;
    uint64_t h = 1469598103934665603ULL;
    for (; *leaf; ++leaf) { h ^= (uint16_t)*leaf; h *= 1099511628211ULL; }
    snprintf(base, cap, "%016llx", (unsigned long long)h);
}
static inline HANDLE gs_shm_cap_create(const char* base) {
    char n[200];
    snprintf(n, sizeof n, "Local\\takedown-gs-cap-%s", base);
    return CreateEventA(NULL, TRUE, FALSE, n);
}
static inline int gs_shm_cap_present(const char* base) {
    char n[200];
    snprintf(n, sizeof n, "Local\\takedown-gs-cap-%s", base);
    HANDLE e = OpenEventA(SYNCHRONIZE, FALSE, n);
    if (!e) return 0;
    CloseHandle(e);
    return 1;
}

#ifdef __cplusplus
}
#endif
