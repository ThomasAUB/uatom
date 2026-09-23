/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * MIT License                                                                     *
 *                                                                                 *
 * Copyright (c) 2026 Thomas AUBERT                                                *
 *                                                                                 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy    *
 * of this software and associated documentation files (the "Software"), to deal   *
 * in the Software without restriction, including without limitation the rights    *
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell       *
 * copies of the Software, and to permit persons to whom the Software is           *
 * furnished to do so, subject to the following conditions:                        *
 *                                                                                 *
 * The above copyright notice and this permission notice shall be included in all  *
 * copies or substantial portions of the Software.                                 *
 *                                                                                 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR      *
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,        *
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE     *
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER          *
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,   *
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE   *
 * SOFTWARE.                                                                       *
 *                                                                                 *
 * github : https://github.com/ThomasAUB/uatom                                     *
 *                                                                                 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

// uatom::Atomic<T> is std::atomic<T> wherever the core can implement it
// without library support. Cores without exclusive access instructions
// (ARMv6-M : Cortex-M0, M0+, M1) have no LDREX/STREX, so the compiler turns
// every read-modify-write - and with clang even plain loads and stores - into
// calls to __atomic_*_N helpers that no bare metal runtime provides. There,
// Atomic<T> is a drop-in replacement that performs each operation with
// interrupts masked through PRIMASK for a handful of cycles.
//
// Define UATOM_IRQ_MASK to 0 or 1 to override the detection.
//
// The PRIMASK path makes an operation atomic against interrupts on the core
// that performs it. Two cases are not covered :
// - multi-core ARMv6-M parts (e.g. RP2040) : masking interrupts doesn't stop
//   the other core, so an Atomic<T> must only be shared between contexts
//   running on the same core.
// - unprivileged code : on an M0+ implementing the unprivileged extension,
//   CPSID and MSR PRIMASK are silently ignored in thread mode running
//   unprivileged, so operations performed there are not atomic.
//
// The inline assembly uses the GNU extended syntax : GCC, clang and Arm
// Compiler 6 (armclang).
#ifndef UATOM_IRQ_MASK
#if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M') && !defined(__ARM_FEATURE_LDREX)
#define UATOM_IRQ_MASK 1
#else
#define UATOM_IRQ_MASK 0
#endif
#endif

namespace uatom {

    namespace detail {

        /**
         * @brief std::atomic look-alike whose operations run in a critical section.
         *
         * @tparam T value type, must be trivially copyable
         * @tparam policy_t provides the critical section and the barrier :
         *   static uint32_t enter();    // mask interrupts, return previous state
         *   static void exit(uint32_t); // restore the state returned by enter()
         *   static void fence();        // hardware memory barrier
         *
         * Scalars of 1, 2 or 4 bytes are loaded and stored with a single
         * access, which the hardware performs atomically, so load() and
         * store() only pay for a barrier. Other types, and every
         * read-modify-write, go through the critical section.
         */
        template<typename T, typename policy_t>
        struct IrqMaskAtomic {

            static_assert(std::is_trivially_copyable<T>::value,
                "Atomic type must be trivially copyable");

            using value_type = T;

            // Nothing ever falls back to a lock or a library call.
            static constexpr bool is_always_lock_free = true;

            constexpr IrqMaskAtomic() noexcept : mValue {} {}
            constexpr IrqMaskAtomic(T inValue) noexcept : mValue(inValue) {}

            IrqMaskAtomic(const IrqMaskAtomic&) = delete;
            IrqMaskAtomic& operator=(const IrqMaskAtomic&) = delete;

            bool is_lock_free() const noexcept { return true; }

            T load(std::memory_order inOrder = std::memory_order_seq_cst) const noexcept {
                T value;
                if constexpr (single_access) {
                    value = *static_cast<const volatile T*>(&mValue);
                }
                else {
                    const CriticalSection cs;
                    copyRepresentation(value, mValue);
                }
                if (inOrder != std::memory_order_relaxed) {
                    policy_t::fence();
                }
                return value;
            }

            void store(T inValue, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                if (inOrder != std::memory_order_relaxed) {
                    policy_t::fence();
                }
                if constexpr (single_access) {
                    *static_cast<volatile T*>(&mValue) = inValue;
                }
                else {
                    const CriticalSection cs;
                    copyRepresentation(mValue, inValue);
                }
                if (inOrder == std::memory_order_seq_cst) {
                    policy_t::fence();
                }
            }

            operator T() const noexcept { return load(); }

            T operator=(T inValue) noexcept {
                store(inValue);
                return inValue;
            }

            T exchange(T inValue, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                return update([&] (T) { return inValue; }, inOrder);
            }

            bool compare_exchange_strong(T& ioExpected, T inDesired,
                std::memory_order inSuccess, std::memory_order inFailure) noexcept {
                const bool fenced = (inSuccess != std::memory_order_relaxed) ||
                    (inFailure != std::memory_order_relaxed);
                if (fenced) {
                    policy_t::fence();
                }
                bool exchanged;
                {
                    const CriticalSection cs;
                    exchanged = sameRepresentation(mValue, ioExpected);
                    if (exchanged) {
                        copyRepresentation(mValue, inDesired);
                    }
                    else {
                        copyRepresentation(ioExpected, mValue);
                    }
                }
                if (fenced) {
                    policy_t::fence();
                }
                return exchanged;
            }

            bool compare_exchange_strong(T& ioExpected, T inDesired,
                std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                return compare_exchange_strong(ioExpected, inDesired, inOrder, inOrder);
            }

            // Never fails spuriously : the critical section cannot be interrupted.
            bool compare_exchange_weak(T& ioExpected, T inDesired,
                std::memory_order inSuccess, std::memory_order inFailure) noexcept {
                return compare_exchange_strong(ioExpected, inDesired, inSuccess, inFailure);
            }

            bool compare_exchange_weak(T& ioExpected, T inDesired,
                std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                return compare_exchange_strong(ioExpected, inDesired, inOrder, inOrder);
            }

            T fetch_add(T inArg, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                static_assert(std::is_integral<T>::value, "fetch_add requires an integral type");
                return update([&] (T v) { return static_cast<T>(v + inArg); }, inOrder);
            }

            T fetch_sub(T inArg, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                static_assert(std::is_integral<T>::value, "fetch_sub requires an integral type");
                return update([&] (T v) { return static_cast<T>(v - inArg); }, inOrder);
            }

            T fetch_or(T inArg, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                static_assert(std::is_integral<T>::value, "fetch_or requires an integral type");
                return update([&] (T v) { return static_cast<T>(v | inArg); }, inOrder);
            }

            T fetch_and(T inArg, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                static_assert(std::is_integral<T>::value, "fetch_and requires an integral type");
                return update([&] (T v) { return static_cast<T>(v & inArg); }, inOrder);
            }

            T fetch_xor(T inArg, std::memory_order inOrder = std::memory_order_seq_cst) noexcept {
                static_assert(std::is_integral<T>::value, "fetch_xor requires an integral type");
                return update([&] (T v) { return static_cast<T>(v ^ inArg); }, inOrder);
            }

        private:

            struct CriticalSection final {
                CriticalSection() : mState(policy_t::enter()) {}
                ~CriticalSection() { policy_t::exit(mState); }
                const uint32_t mState;
            };

            // A volatile access to a class type isn't possible, so structs
            // always go through the critical section, even when small.
            static constexpr bool single_access = std::is_scalar<T>::value &&
                ((sizeof(T) == 1) || (sizeof(T) == 2) || (sizeof(T) == 4));

            // Compares the object representations, as std::atomic does. An
            // inline loop rather than memcmp, so that no library call runs
            // with interrupts masked.
            static bool sameRepresentation(const T& inA, const T& inB) noexcept {
                if constexpr (std::is_integral<T>::value || std::is_enum<T>::value ||
                    std::is_pointer<T>::value) {
                    return inA == inB;
                }
                else {
                    const auto* a = reinterpret_cast<const unsigned char*>(&inA);
                    const auto* b = reinterpret_cast<const unsigned char*>(&inB);
                    for (size_t i = 0; i < sizeof(T); ++i) {
                        if (a[i] != b[i]) {
                            return false;
                        }
                    }
                    return true;
                }
            }

            // Copies the object representation. Assigning a struct lets the
            // compiler call memcpy (GCC does at -O0 for under-aligned types),
            // so non-scalars are copied by an inline loop whose volatile
            // accesses can't be turned back into a library call. The loop
            // uses the widest unit that the alignment and size of T allow.
            static void copyRepresentation(T& outDst, const T& inSrc) noexcept {
                if constexpr (std::is_scalar<T>::value) {
                    outDst = inSrc;
                }
                else {
                    using unit_t = typename std::conditional<
                        (alignof(T) % 4 == 0) && (sizeof(T) % 4 == 0), uint32_t,
                        typename std::conditional<
                            (alignof(T) % 2 == 0) && (sizeof(T) % 2 == 0), uint16_t,
                            uint8_t>::type>::type;
#if defined(__GNUC__) || defined(__clang__)
                    typedef unit_t __attribute__((may_alias)) alias_t;
#else
                    typedef unit_t alias_t;
#endif
                    auto* dst = reinterpret_cast<volatile alias_t*>(&outDst);
                    const auto* src = reinterpret_cast<const volatile alias_t*>(&inSrc);
                    for (size_t i = 0; i < sizeof(T) / sizeof(unit_t); ++i) {
                        dst[i] = src[i];
                    }
                }
            }

            template<typename F>
            T update(F&& inOperation, std::memory_order inOrder) noexcept {
                if (inOrder != std::memory_order_relaxed) {
                    policy_t::fence();
                }
                T previous;
                {
                    const CriticalSection cs;
                    copyRepresentation(previous, mValue);
                    const T next = inOperation(previous);
                    copyRepresentation(mValue, next);
                }
                if (inOrder != std::memory_order_relaxed) {
                    policy_t::fence();
                }
                return previous;
            }

            T mValue;
        };

#if UATOM_IRQ_MASK
        struct PrimaskPolicy final {
            // The "memory" clobbers keep the compiler from moving accesses
            // out of the critical section.
            static uint32_t enter() {
                uint32_t primask;
                __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r"(primask) :: "memory");
                return primask;
            }

            static void exit(uint32_t inPrimask) {
                __asm volatile ("msr primask, %0" :: "r"(inPrimask) : "memory");
            }

            static void fence() {
                __asm volatile ("dmb" ::: "memory");
            }
        };
#endif

    }

#if UATOM_IRQ_MASK
    template<typename T>
    using Atomic = detail::IrqMaskAtomic<T, detail::PrimaskPolicy>;
#else
    template<typename T>
    using Atomic = std::atomic<T>;
#endif

}
