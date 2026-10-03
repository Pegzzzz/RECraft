// memcheck.h - "can this game memory be read?" without a kernel call for every pointer.
//
// Each VirtualQuery answer (a committed, readable region) is remembered in a small per-thread cache;
// later checks inside the same region are answered from it. The cache is cleared at the start of each
// hook (mem::Reset), so it only lives for one pass over the game's data on one thread - the game can't
// free memory in the middle of our own code on its own thread.
// In big fights this replaced thousands of VirtualQuery calls per frame (every bone of every enemy).
#pragma once
#include <windows.h>
#include <cstdint>

namespace mem {

struct Reg { uintptr_t b, e; };
extern __thread Reg t_reg[16];
extern __thread int t_n, t_next;

inline void Reset() { t_n = 0; t_next = 0; }

// The whole range [p, p+n) is committed and readable (it may span several regions).
inline bool Readable(const void* p, size_t n) {
    uintptr_t a = (uintptr_t)p, z = a + n;
    if (a < 0x10000 || z < a) return false;
    for (int i = 0; i < t_n; i++) if (a >= t_reg[i].b && z <= t_reg[i].e) return true;
    uintptr_t cur = a;
    while (cur < z) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery((void*)cur, &m, sizeof m)) return false;
        if (m.State != MEM_COMMIT || !m.Protect || (m.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        uintptr_t rb = (uintptr_t)m.BaseAddress, re = rb + m.RegionSize;
        t_reg[t_next] = {rb, re}; t_next = (t_next + 1) & 15; if (t_n < 16) t_n++;
        if (re <= cur) return false;
        cur = re;
    }
    return true;
}

} // namespace mem
