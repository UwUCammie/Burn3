#pragma once

// The C library's qsort (BSD's, as the game was linked with): median of
// three (of nine above 40 elements) quicksort with three-way partitioning,
// insertion sort below 7 elements and for already ordered runs, recursion on
// the left part and a loop on the right. It is not stable, so game code
// relies on its exact order of comparisons and swaps; this follows them.
//
// Elements are addressed by offset: `compare(a, b)` compares the elements at
// offsets a and b (negative, zero or positive) and `swap(a, b, bytes)`
// exchanges `bytes` bytes at a and b.

#include <cstdint>

namespace takedown::engine {

struct QsortCounts {
    std::uint32_t comparisons{};
    std::uint32_t swaps{};
    std::uint32_t swapped_bytes{};
};

template <class Compare, class Swap>
void bsd_qsort(std::uint32_t base, std::uint32_t count, const std::uint32_t size, Compare&& compare, Swap&& swap,
               QsortCounts& counts) {
    const auto cmp = [&](const std::uint32_t a, const std::uint32_t b) {
        ++counts.comparisons;
        return compare(a, b);
    };
    const auto exchange = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t bytes) {
        ++counts.swaps;
        counts.swapped_bytes += bytes;
        swap(a, b, bytes);
    };
    const auto med3 = [&](const std::uint32_t a, const std::uint32_t b, const std::uint32_t c) {
        if (cmp(a, b) < 0) {
            if (cmp(b, c) < 0) {
                return b;
            }
            return cmp(a, c) < 0 ? c : a;
        }
        if (cmp(b, c) > 0) {
            return b;
        }
        return cmp(a, c) < 0 ? a : c;
    };
    const auto insertion_sort = [&] {
        for (std::uint32_t pm = base + size; pm < base + count * size; pm += size) {
            for (std::uint32_t pl = pm; pl > base && cmp(pl - size, pl) > 0; pl -= size) {
                exchange(pl, pl - size, size);
            }
        }
    };
    for (;;) {
        bool swapped = false;
        if (count < 7U) {
            insertion_sort();
            return;
        }
        std::uint32_t pm = base + (count / 2U) * size;
        if (count > 7U) {
            std::uint32_t pl = base;
            std::uint32_t pn = base + (count - 1U) * size;
            if (count > 40U) {
                const std::uint32_t d = (count / 8U) * size;
                pl = med3(pl, pl + d, pl + 2U * d);
                pm = med3(pm - d, pm, pm + d);
                pn = med3(pn - 2U * d, pn - d, pn);
            }
            pm = med3(pl, pm, pn);
        }
        exchange(base, pm, size);
        std::uint32_t pa = base + size;
        std::uint32_t pb = pa;
        std::uint32_t pc = base + (count - 1U) * size;
        std::uint32_t pd = pc;
        for (;;) {
            int r;
            while (pb <= pc && (r = cmp(pb, base)) <= 0) {
                if (r == 0) {
                    swapped = true;
                    exchange(pa, pb, size);
                    pa += size;
                }
                pb += size;
            }
            while (pb <= pc && (r = cmp(pc, base)) >= 0) {
                if (r == 0) {
                    swapped = true;
                    exchange(pc, pd, size);
                    pd -= size;
                }
                pc -= size;
            }
            if (pb > pc) {
                break;
            }
            exchange(pb, pc, size);
            swapped = true;
            pb += size;
            pc -= size;
        }
        if (!swapped) {
            insertion_sort();
            return;
        }
        const std::uint32_t pn = base + count * size;
        std::uint32_t r = pa - base < pb - pa ? pa - base : pb - pa;
        if (r > 0U) {
            exchange(base, pb - r, r);
        }
        r = pd - pc < pn - pd - size ? pd - pc : pn - pd - size;
        if (r > 0U) {
            exchange(pb, pn - r, r);
        }
        if ((r = pb - pa) > size) {
            bsd_qsort(base, r / size, size, compare, swap, counts);
        }
        if ((r = pd - pc) > size) {
            base = pn - r;
            count = r / size;
            continue;
        }
        return;
    }
}

} // namespace takedown::engine
