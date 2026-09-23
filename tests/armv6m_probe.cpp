// Compiled for Cortex-M0+ by the CI (not run) : the object must not reference
// any __atomic_* helper, since no bare metal runtime provides them.

#include "uatom.hpp"

static_assert(UATOM_IRQ_MASK, "the probe must be built for ARMv6-M");

namespace {
    struct Pair16 { uint16_t a, b; };
    struct Wide { uint32_t a, b; };
}

uatom::Atomic<bool> gFlag;
uatom::Atomic<uint8_t> gByte;
uatom::Atomic<uint32_t> gWord;
uatom::Atomic<Pair16> gPair;
uatom::Atomic<Wide> gWide;
uatom::Atomic<uint64_t> gLong;

uint32_t probe() {
    gFlag.store(true, std::memory_order_release);
    gByte.fetch_add(1);
    gWord.fetch_or(1);
    gWord.fetch_and(~2u);
    gWord.fetch_xor(4);
    gWord.fetch_sub(1, std::memory_order_relaxed);
    uint32_t expected = 3;
    gWord.compare_exchange_weak(expected, 4);
    Pair16 p { 1, 2 };
    gPair.compare_exchange_strong(p, Pair16 { 3, 4 });
    gWide.store(Wide { 1, 2 });
    gLong.fetch_add(5);
    return gWord.exchange(0) + gFlag.load() + gPair.load().a + gWide.load().b +
        static_cast<uint32_t>(gLong.load());
}
