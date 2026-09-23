#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "uatom.hpp"

#include <mutex>
#include <thread>

// The PRIMASK policy only builds for ARMv6-M. On the host, IrqMaskAtomic is
// exercised with a policy that takes a mutex in place of masking interrupts :
// it makes the critical section real for threads, and counts the critical
// sections so the tests can check which operations need one.
namespace {

    struct MutexPolicy {
        static inline std::mutex sMutex;
        static inline int sDepth = 0;
        static inline int sEntries = 0;

        static uint32_t enter() {
            sMutex.lock();
            ++sDepth;
            ++sEntries;
            return 0;
        }

        static void exit(uint32_t) {
            --sDepth;
            sMutex.unlock();
        }

        static void fence() {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    };

    template<typename T>
    using TestAtomic = uatom::detail::IrqMaskAtomic<T, MutexPolicy>;

    struct Pair16 {
        uint16_t a;
        uint16_t b;
    };

    struct Wide {
        uint32_t a;
        uint32_t b;
    };

}

TEST_CASE("IrqMaskAtomic - integral operations") {

    TestAtomic<uint32_t> value { 5 };
    CHECK(value.is_always_lock_free);
    CHECK(value.load() == 5);

    value.store(7);
    CHECK(value.load(std::memory_order_relaxed) == 7);

    CHECK(value.fetch_add(3) == 7);
    CHECK(value.fetch_sub(2) == 10);
    CHECK(value.fetch_or(0x100) == 8);
    CHECK(value.fetch_and(0x10F) == 0x108);
    CHECK(value.fetch_xor(0x001) == 0x108);
    CHECK(value.exchange(42) == 0x109);
    CHECK(value == 42u);

    value = 1;
    CHECK(value.load() == 1);

    // wraps like std::atomic
    TestAtomic<uint8_t> small { 0xFF };
    CHECK(small.fetch_add(1) == 0xFF);
    CHECK(small.load() == 0);

    TestAtomic<bool> flag;
    CHECK_FALSE(flag.load());
    CHECK_FALSE(flag.exchange(true));
    CHECK(flag.load());
}

TEST_CASE("IrqMaskAtomic - compare exchange") {

    TestAtomic<uint32_t> value { 10 };

    uint32_t expected = 11;
    CHECK_FALSE(value.compare_exchange_strong(expected, 20));
    CHECK(expected == 10);
    CHECK(value.load() == 10);

    CHECK(value.compare_exchange_strong(expected, 20));
    CHECK(value.load() == 20);

    // weak never fails spuriously
    expected = 20;
    CHECK(value.compare_exchange_weak(expected, 30,
        std::memory_order_release, std::memory_order_relaxed));
    CHECK(value.load() == 30);
}

TEST_CASE("IrqMaskAtomic - critical sections") {

    TestAtomic<uint32_t> word { 0 };
    TestAtomic<Wide> wide { Wide { 1, 2 } };

    // single access loads and stores don't need one
    MutexPolicy::sEntries = 0;
    word.store(1);
    (void)word.load();
    CHECK(MutexPolicy::sEntries == 0);

    // read-modify-writes always do
    word.fetch_or(2);
    uint32_t expected = 3;
    word.compare_exchange_strong(expected, 4);
    CHECK(MutexPolicy::sEntries == 2);

    // a value too wide for a single access needs one even to be read
    MutexPolicy::sEntries = 0;
    wide.store(Wide { 3, 4 });
    const Wide w = wide.load();
    CHECK(MutexPolicy::sEntries == 2);
    CHECK(w.a == 3);
    CHECK(w.b == 4);

    CHECK(MutexPolicy::sDepth == 0);
}

TEST_CASE("IrqMaskAtomic - trivially copyable structs") {

    TestAtomic<Pair16> pair { Pair16 { 1, 2 } };

    Pair16 expected { 1, 2 };
    CHECK(pair.compare_exchange_strong(expected, Pair16 { 3, 4 }));

    expected = Pair16 { 9, 9 };
    CHECK_FALSE(pair.compare_exchange_strong(expected, Pair16 { 5, 6 }));
    CHECK(expected.a == 3);
    CHECK(expected.b == 4);

    const Pair16 previous = pair.exchange(Pair16 { 7, 8 });
    CHECK(previous.a == 3);
    CHECK(pair.load().b == 8);

    TestAtomic<Wide> wide { Wide { 1, 2 } };
    Wide wideExpected { 1, 2 };
    CHECK(wide.compare_exchange_strong(wideExpected, Wide { 5, 6 }));
    CHECK(wide.load().b == 6);
}

TEST_CASE("IrqMaskAtomic - concurrent read-modify-writes") {

    constexpr int thread_count = 4;
    constexpr int iterations = 20000;

    TestAtomic<uint32_t> counter { 0 };
    TestAtomic<uint32_t> bits { 0 };

    std::thread threads[thread_count];
    for (int t = 0; t < thread_count; ++t) {
        threads[t] = std::thread(
            [&, t] {
                for (int i = 0; i < iterations; ++i) {
                    counter.fetch_add(1, std::memory_order_relaxed);
                }
                bits.fetch_or(1u << t);
            }
        );
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK(counter.load() == thread_count * iterations);
    CHECK(bits.load() == (1u << thread_count) - 1u);
}

TEST_CASE("Atomic - host is std::atomic") {
    // Only ARMv6-M gets the interrupt masking implementation.
    static_assert(std::is_same<uatom::Atomic<uint32_t>, std::atomic<uint32_t>>::value,
        "the host must use std::atomic");
    CHECK(uatom::Atomic<uint32_t>::is_always_lock_free);
}
