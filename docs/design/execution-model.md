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

## Who dispatches an interrupt

Record and replay differ here, and it is the sharpest edge in the design.

**On hardware**, the NVIC dispatches. The shim is not consulted and cannot
be: an interrupt arrives between two instructions of the compiler's choosing.
So `HalOps::poll_irq` returns `kNoIrq` forever and each ISR announces itself
by calling `hal_record_irq_entry(vector)` in its prologue.

**On replay**, there is no NVIC, so the shim dispatches from the trace —
`poll_irq` serves the next `EV_IRQ_ENTER` and the shim calls the handler.
But the shim only gets to run at an MMIO access, so that is the only place
an interrupt can be re-entered.

The two do not line up, and the consequence is concrete: a trace recorded on
silicon whose interrupt landed in the middle of a computation has no
replayable point to land on. Replay reports a divergence. That is the right
failure — it says "I cannot reproduce this" rather than reproducing
something else — but it is a failure, and it is what stands between this
working in simulation and working on a device.

Closing it means recording where the interrupt actually landed, not just
that it did: the return address from the exception frame, plus something on
the host that can stop there. That is the next piece of real work.

Under the simulator the question does not arise, because the simulator has
no way to interrupt except through the shim. Traces recorded there replay
exactly, which is why the golden tests pass and why they are not by
themselves evidence that hardware traces will.

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

Timestamps must be monotonic, and the writer rejects a backwards one rather
than accepting a trace whose every subsequent delta would be wrong.

`DWT->CYCCNT` is 32 bits and wraps every 25.6 seconds at 168 MHz, so
`ClockExtender` widens it: a reading lower than the previous one means a
wrap, so bump a high word. It is exact as long as sampling is more frequent
than the wrap period, and every recorded MMIO access is a sample.

Firmware that goes quiet for longer than a period — deep sleep, a long
computation, a halted debugger — can skip a wrap, and the counter alone
cannot distinguish one wrap from two: the evidence is identical. Nothing
clever fixes that; it needs a second, slower clock. `suspicious()` counts
sample gaps beyond half a period so the doubt is reported rather than
buried.

## Divergence

A replay backend reports a divergence when the firmware asks for something
the trace does not have: a read at an address the recording did not read, an
event out of turn, an access past the end. The first report latches;
everything after it is a consequence.

Divergence is the tool's only honest failure mode. Any firmware change,
build mismatch or model breakage shows up as one, which is why the replay
engine checks aggressively rather than tolerating near-misses.

## Losing data

A device recording faster than its link can carry runs out of buffer. The
design question is not how to avoid it — you cannot, in general — but what
to do when it happens.

Three options, and only one of them is honest:

1. **Stop recording at the first loss.** The trace stays contiguous and
   short. You lose everything after the first burst, which is usually the
   part you wanted.
2. **Drop and carry on silently.** The trace has a hole. Every surviving
   block still passes its CRC, so it parses perfectly, and replay diverges
   somewhere downstream for reasons that look exactly like a firmware bug.
   This is the worst outcome available and the easiest one to implement.
3. **Drop, carry on, and record that you did.** What `rewind` does.

Each block carries a sequence number. A block the sink refuses is counted,
the sequence still advances, and recording continues. A reader comparing
sequence numbers sees the gap immediately, reports its size, and stops rather
than decoding the next block's deltas against state that belongs to a block
that never arrived.

Four bytes per block against an afternoon of chasing a bug that is not there.

The device can also see it at the time: `Recorder::healthy()` is false,
`blocks_lost()` says how many, and `ring().high_water()` says how close the
buffer came to coping. `rewind sizing` sweeps capacities and reports all
three.

## State that is not recorded

The firmware's RAM is not in the trace. Replay reconstructs it by
re-executing, which is what makes traces small — kilobytes, not megabytes.

The consequence is that replay must start from the same initial state as the
recording. Right now that means starting from reset. Recording from an
arbitrary point would need an initial RAM snapshot in the header, which is
straightforward and not yet done.
