# rewind

Deterministic record & replay for bare-metal firmware.

Record what crosses the hardware boundary on a device; replay it bit-for-bit
on your workstation, with no hardware and no simulator involved. A failure
that happens once a week in the field becomes a file you can reproduce on
demand.

**Status: milestone 0.** The core works end to end and is covered by tests,
entirely on the host. It has not run on real silicon yet — see
[What this does not do yet](#what-this-does-not-do-yet).

## The problem

Most of the time spent debugging firmware goes on bugs that will not
reproduce: a race with an ISR, a peripheral that answers a few cycles later
than usual, a watchdog that fires once every three days at a customer's site.
The existing tools — SystemView, Tracealyzer, ITM/SWO — are *observation*
traces. They tell you what happened. They do not let you run it again.

## The idea

Bare-metal firmware is a deterministic function of its inputs, and those
inputs are a small, enumerable set:

1. reads of peripheral registers,
2. interrupt arrivals (which vector, at which cycle),
3. blocks DMA deposits in RAM,
4. reads of the clock.

Record those at the boundary between the firmware and the hardware, and the
same source, recompiled for the host, re-executes identically. Mozilla's `rr`
does this for x86 userspace; nothing open-source does it for a microcontroller.

## Demo

Run the example firmware under a thousand different interrupt-arrival
schedules and count how many fail:

```console
$ rewind hunt --from 1 --to 1000 --save bug.rwd
seed 2        FAIL  consumed 23 of 24 bytes  (32073 byte trace)

419 of 1000 runs failed (41.9%)
wrote the seed 2 failure to bug.rwd -- it now fails identically, every time:
  rewind replay bug.rwd
```

Forty-two percent. Miss it in testing and it ships. But the failing run is
now a 32 KB file, and that file is deterministic:

```console
$ rewind replay bug.rwd
replayed 6095 of 6096 events -- no simulator, no hardware
consumed 23 bytes  checksum 0x1EEC8173  state hash 0x3D74C29F6958D8CC
```

Every time. Then find out why:

```console
$ rewind dump bug.rwd
      6186  read   0x40001000 UART0.SR      0x00000003
      6189  read   0x40001004 UART0.DR      0x000000D3   <- ISR takes byte 19
      6192  read   0x40000000 SYSTICK.CNT   0x00001830
      6195  write  0x40002000 GPIO0.ODR     0x00000013   <- main loop drains it
      6195  irq    vector 1   UART_RX                    <- byte 20 lands HERE
      6198  read   0x40001000 UART0.SR      0x00000003
      6201  read   0x40001004 UART0.DR      0x00000091   <- and is buffered
      6204  read   0x40000000 SYSTICK.CNT   0x0000183C   <- ...then idle. Why?
```

The interrupt arrived *inside* the drain loop. The next line of firmware is
`pending = 0` — clearing a counter the ISR had just incremented — so byte 20
sits in the ring buffer and the main loop goes back to idling. One line, in
[`firmware/src/rx_pump.cpp`](firmware/src/rx_pump.cpp):

```cpp
g_state.pending = 0;       // BUG: erases anything the ISR added
                           // while the drain loop was running
g_state.pending -= n;      // what it should be, under a critical section
```

## Architecture

Two worlds, kept apart by the build system rather than by discipline.

| Directory   | What                                        | Standard |
|-------------|---------------------------------------------|----------|
| `core/`     | Trace format: varint, CRC-32, writer, reader | C++98 |
| `target/`   | The HAL shim — MMIO and interrupt interception | C++98 |
| `sim/`      | A toy MCU: cycle counter, UART, GPIO         | C++98 |
| `firmware/` | Example firmware, in buggy and fixed variants | C++98 |
| `host/`     | Replay engine, session orchestration, CLI     | C++17 |
| `tests/`    | 45 cases, 18 606 assertions                   | C++17 |

Everything that conceptually ships on the device is strict C++98 with no
heap, no exceptions and no RTTI, and CMake enforces it with
`-std=c++98 -pedantic-errors`. An `auto` in `core/` is a build failure, not a
code-review argument.

## Build and test

```console
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/rewind_tests
ctest --test-dir build
```

Cross-compiling the two libraries that would ship on the device:

```console
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DREWIND_BUILD_TESTS=OFF
cmake --build build-arm --target rewind_core rewind_target
```

## How it works

The firmware never touches a register directly. It goes through
`mmio_read32` / `mmio_write32` and receives interrupts through the shim,
which runs in one of three modes:

| Mode | Backend | Records |
|------|---------|---------|
| `MODE_OFF` | real hardware | nothing — this is production |
| `MODE_RECORD` | real hardware | every input, appended to a trace |
| `MODE_REPLAY` | **a trace** | nothing |

The symmetry is the point. Record and replay run identical firmware down
identical code paths; the only difference is where the bytes come from. So
any deviation surfaces immediately as a mismatched address or an event out of
turn — a divergence, reported loudly:

```console
$ rewind replay bug.rwd --iters 12000
DIVERGED: read of 0x40000000 past the end of the recording
          (the recorded run stopped after 6095 events)
```

### Trace format

A 32-byte header, then CRC-checked blocks of varint-packed events. Blocking
means a trace cut short by a brown-out still yields every whole block that
made it out, and the reader stops cleanly at the ragged tail instead of
reporting garbage.

Timestamps and addresses are both stored as deltas. The address delta is
ZigZag-coded and matters more than it looks: `0x40000000` costs five bytes in
LEB128 *every time*, but firmware poke the same handful of registers over and
over, so the delta is usually zero and costs one. That single change took the
example trace from 9.2 bytes per event to **5.3** — on a device shipping
traces over a 115200 baud UART, the difference between viable and not.

The header carries a `build_id`. Replaying a trace against a different
firmware build is the fastest way to spend an afternoon chasing a bug that is
not there, so it is refused.

## What this does not do yet

Stated plainly, because the gap between this and the pitch is real:

- **No hardware.** The shim is ready and the format is portable, but there is
  no real MMIO backend, no transport (SWO/RTT/UART) and no on-target ring
  buffer drain. Nothing here has run on silicon.
- **No reverse execution.** The headline feature is still a design: replay is
  deterministic, so `fork()`-based checkpoints give reverse-step cheaply, but
  none of it is written.
- **Interrupts land only at MMIO accesses.** Real silicon can interrupt at any
  instruction boundary. This model is coarser — enough to catch the whole
  class of bugs where an ISR lands inside a read-modify-write on shared
  state, which is the class worth catching first, but not a complete model.
- **No DMA.** The event type is designed and not implemented.
- **No 32-bit clock wraparound handling.** `DWT->CYCCNT` wraps; extending it
  to the 64 bits the format expects is not written.
- **No GUI.** Timeline scrubbing and trace diffing are the eventual Qt layer.
- **Overhead is unmeasured.** The <2% target is a design goal with no number
  behind it yet.

## Roadmap

1. Real MMIO backend plus an on-target ring buffer and a UART drain.
2. Measure the recording overhead on hardware and hold it in CI.
3. `fork()` checkpoints and reverse-step.
4. DMA events and clock extension.
5. Qt timeline and two-trace diffing.

## License

Apache-2.0. See [LICENSE](LICENSE).

---

There is a second idea parked in [`docs/ideas/bitwise.md`](docs/ideas/bitwise.md):
automatic protocol reverse engineering from bus captures. Not started.
