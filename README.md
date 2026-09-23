![build status](https://github.com/ThomasAUB/uatom/actions/workflows/build.yml/badge.svg)
[![License](https://img.shields.io/github/license/ThomasAUB/uatom)](LICENSE)

# uAtom

Portable `std::atomic` for microcontrollers, including the ones that can't do it.

- single header file
- drop-in replacement for `std::atomic`
- no library call, no lock, no heap allocation
- zero cost where `std::atomic` already works

## Why

Cortex-M0, M0+ and M1 (ARMv6-M) have no exclusive access instructions
(`LDREX`/`STREX`). On these cores the compiler turns atomic read-modify-writes,
and with clang even plain atomic loads and stores, into calls to
`__atomic_*_N` helpers that no bare metal runtime provides : the code compiles
and then fails to link.

`uatom::Atomic<T>` is `std::atomic<T>` everywhere else. On ARMv6-M it performs
each operation with interrupts masked through `PRIMASK`, for a handful of
cycles.

## Example

```cpp
#include "uatom.hpp"

uatom::Atomic<uint32_t> gPending { 0 };

// interrupt handler
void onEvent(uint8_t inId) {
    gPending.fetch_or(1u << inId, std::memory_order_release);
}

// main loop
void process() {
    uint32_t pending = gPending.exchange(0, std::memory_order_acq_rel);
    while (pending) {
        // ...
    }
}
```

## Cost on ARMv6-M

| Operation | Implementation |
|---|---|
| `load` / `store` of a 1, 2 or 4 byte scalar | single access + `dmb` |
| read-modify-write, compare-exchange | `mrs primask` / `cpsid i` ... `msr primask` |
| any access to a struct or an 8 byte value | same critical section |

`compare_exchange_weak` never fails spuriously.

## Limitations

On ARMv6-M, masking interrupts makes an operation atomic on the core that
performs it only :

- **multi-core parts** (e.g. RP2040) : an `Atomic` must not be shared between cores.
- **unprivileged code** : on an M0+ implementing the unprivileged extension,
  `cpsid` is ignored in unprivileged thread mode, so operations performed
  there are not atomic.

The inline assembly uses the GNU extended syntax : GCC, clang and Arm Compiler 6.

## Configuration

The interrupt masking implementation is selected on M-profile cores without
`__ARM_FEATURE_LDREX`. Define `UATOM_IRQ_MASK` to `0` or `1` to override it.

## CMake

```cmake
add_subdirectory(uatom)
target_link_libraries(my_target uatom)
```
