# Execution model

What `rewind` assumes about the firmware it records, where those assumptions
are exact, and where they are approximations. The approximations are the
interesting part: they bound what the tool can and cannot catch.

## The determinism argument

Firmware is a state machine driven by its inputs. Fix the inputs and the
state machine is a pure function, and a pure function replays.

For bare-metal code the input set is small and closed:

| Input | Event | Status |
|---|---|---|
| Peripheral register read | `EV_MMIO_READ` | implemented |
| Interrupt arrival | `EV_IRQ_ENTER` | implemented |
| Clock read | an MMIO read of the timer | implemented |
| DMA-deposited memory | `EV_DMA` | designed, not implemented |

Everything else the firmware does — arithmetic, control flow, writes to its
own RAM — is a consequence of those inputs and needs no recording.

Register *writes* are output, not input, so they are not needed for a correct
replay. They are recorded anyway, under `kFlagRecordWrites`, because during
replay they are exactly what proves the replay did not silently diverge.
Clearing the flag halves the trace and loses that check.

## Where interrupts land

Real silicon can take an interrupt at almost any instruction boundary.
`rewind` dispatches at MMIO accesses only — the shim drains pending vectors
after every `mmio_read32` and `mmio_write32`.

This is a genuine approximation, and it cuts both ways.

**What it catches.** Every bug where an ISR lands inside a non-atomic
read-modify-write on state it shares with the main loop, *provided the main
loop touches a peripheral inside the window*. In practice it usually does:
draining a buffer means doing something with the bytes, and doing something
usually means a peripheral. This is the single most common concurrency bug in
embedded C++ and the one the example firmware demonstrates.

**What it misses.** A race whose entire window is pure computation — a
shared counter incremented and stored with no peripheral access in between —
is invisible to this model. A finer model would need instrumentation at
basic-block granularity, which costs far more than 2% and is a different
project.

Handlers are non-reentrant: while one is running, the shim dispatches no
others. Real hardware nests by priority. Modelling that needs a priority
scheme this does not have, and non-reentrant handlers are the common
configuration anyway.

## Critical sections

`hal_irq_disable` / `hal_irq_enable` nest, and are the moral equivalent of
`PRIMASK`. While masked, no interrupt is dispatched **and none is recorded**,
so a critical section looks the same in the trace as it did on the wire.

This matters for replay fidelity. If a masked interrupt were recorded, replay
would deliver it at a point the recorded run never took, and the divergence
would be an artefact of the tool rather than a property of the firmware.

Note the consequence: a critical section that brackets no peripheral access
has no observable effect in this model. Two builds differing only by such a
section produce identical traces. That is a real limit —
`tests/test_determinism.cpp` asserts it rather than papering over it — and it
is why the trace header carries a `build_id`.

## Timestamps

The shim records the backend's notion of `now` after each access. On the
target that is `DWT->CYCCNT`; in the simulator it is a cycle counter.

`hal_now()` is for instrumentation only. Under `MODE_REPLAY` it reports the
timestamp of the last consumed event, which tracks the recorded clock at
every MMIO access but not between them. Firmware that needs the time must
read its timer through `mmio_read32`, which is recorded and therefore exact.

Timestamps must be monotonic. `DWT->CYCCNT` is 32 bits and wraps; the trace
format expects 64, and extending one to the other is not yet written. The
writer rejects a backwards timestamp rather than accepting a trace whose
every subsequent delta would be wrong.

## Divergence

A replay backend reports a divergence when the firmware asks for something
the trace does not have: a read at an address the recording did not read, an
event out of turn, an access past the end. The first report latches;
everything after it is a consequence.

Divergence is the tool's only honest failure mode. Any firmware change,
build mismatch or model breakage shows up as one, which is why the replay
engine checks aggressively rather than tolerating near-misses.

## State that is not recorded

The firmware's RAM is not in the trace. Replay reconstructs it by
re-executing, which is what makes traces small — kilobytes, not megabytes.

The consequence is that replay must start from the same initial state as the
recording. Right now that means starting from reset. Recording from an
arbitrary point would need an initial RAM snapshot in the header, which is
straightforward and not yet done.
