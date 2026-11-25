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
| Interrupt arrival | `EV_IRQ_ENTER`, `EV_IRQ_AT` | recorded; replayed at MMIO accesses only |
| Clock read | an MMIO read of the timer | implemented |
| DMA-deposited memory | `EV_DMA` | implemented: recorded when the firmware reads it |

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
that it did, and having something on the host that can stop there.

### Recording where it landed

Done. A vector table entry declared with `RWD_CORTEX_M_ISR(name, vector)` is a
short naked trampoline (six instructions): pick the stack the frame went to from
bit 2 of `EXC_RETURN`, hand the frame and the vector number to
`rwd_cortex_m_isr_entry`, and tail-branch. That reads the PC stacked in the
exception frame and records it as `EV_IRQ_AT`, then runs the handler
registered with `hal_set_isr`. `LR` is untouched, so the C++ return is the
exception return.

It is run on an emulated Cortex-M4 (QEMU `mps2-an386`), not on a part, by
`tests/qemu/run.sh` and by CI: an interrupt pended by a store is recorded with
the address of the very next instruction, on the main stack and on the process
stack, and a trampoline that ignores `EXC_RETURN` is built alongside and must
fail the process-stack case, or the check would prove nothing. The emulator
only shows the frame arithmetic is right. It says nothing about timing, and
QEMU's NVIC model has no `DWT->CYCCNT`, so timestamps there come from
elsewhere.

### Stopping on it

The address is a location in the *device* binary. Replay runs the firmware
compiled for the *host*, where that address means nothing, so there is nothing
to compare it with. What replay needs is a coordinate for "where in the run"
that is the same in both builds.

The existing one is the order of MMIO accesses, and it is exact but coarse:
it says the interrupt came between access *k* and *k+1*, not where in the
computation between them. The refinement is more places to stop.

### Probe points

Implemented. `RWD_SITE(id)` in the firmware is one more crossing of the shim,
like an MMIO access: it is recorded as `EV_SITE`, replay consumes it in trace
order and checks the label (a missing site, an extra one or a different label
is a divergence like any other), and a pending interrupt is delivered right
after it. The label is the firmware's own, identical on the device and the
host, so nothing has to be translated between builds.

```cpp
const u32 tmp = g_shared;
RWD_SITE(7);                  // an interrupt may land here
g_shared = tmp + 1;
```

`tests/test_sites.cpp` is the demonstration. A load-add-store with no register
access in it cannot lose an update without a probe point: across sixty seeds
the interrupt never lands in the window. With one inside it, about half the
seeds lose an update and half do not (32 and 28 when this was written): an
intermittent bug. A run that lost one replays from its trace alone and loses
the same one. A critical
section still keeps the interrupt out, probe point or not.

How exactly a replay can reproduce a race is how densely the sites are placed
around the code that races. An interrupt that landed between two crossings is
replayed after the earlier one, so a window with no site in it is still out of
reach. A site costs one event in the trace (about three bytes) and, outside
recording, a call and a branch; `REWIND_DISABLE_SITES` removes it.

The example firmware uses one. Its `unguarded` variant is the fix that is not
one: it subtracts what it handled, outside a critical section. `rewind hunt
--variant unguarded` finds it on 85 of 1000 seeds, and every one of those has
an interrupt directly after the site (`tests/test_unguarded.cpp`). Only that
variant crosses the site, so the other two variants and the numbers quoted for
them are unchanged.

### Checking the density

On a part, an interrupt that landed between two crossings is replayed after the
earlier one, and nothing in replay itself says whether that was close enough.
`rewind landings` does. The trace already carries the clock on every event, so
the distance from the last crossing to the interrupt -- the slack, in cycles --
needs no return address: it is how much code ran before the hardware took the
interrupt and runs after the handler in replay. The report lists the interrupts
whose slack is over a threshold, with the crossing each follows and, when the
trace has one, the return address, which says where that code is
(`arm-none-eabi-addr2line -e firmware.elf <address>`).

It is a diagnostic, not a verdict. Slack says code ran in the gap, not that the
code touches anything the handler shares: an interrupt that lands while the
firmware idles has a large slack and costs nothing. And the default threshold
(64 cycles, above a Cortex-M's entry latency and a handler prologue) is a
guess: nothing here has been calibrated on a part, and a simulator trace has no
slack at all, because the simulator can only interrupt right after a crossing.
`tests/test_landings.cpp` builds hardware-shaped traces by hand for that
reason.

### Finding the landing

Past that, the trace is itself an oracle. A trace recorded without its probe
points (`kFlagNoSites`: compiled out on the device, which is the normal case on
a part) says that an interrupt came after crossing A and before crossing B, and
no more. In replay the same `RWD_SITE`s are then not crossings to match against
the trace but *candidate* places to deliver it, and `rewind locate` tries them.

Each interrupt is held back until a number of probe points have gone by since
the last crossing (0 is the default: right after the crossing). The recorded
reads and writes that follow are the check: a placement that matches the real
landing reproduces them, and one that does not makes the firmware read or
write something the trace does not have, at which point replay diverges. The
search is depth-first with backtracking over the interrupts in order, and each
placement is judged by where the *first* divergence happens (the firmware keeps
running after one, and its loop can fall back into step by luck, so the cursor
at the end of a run says nothing). A placement fits if the replay gets past the
next interrupt or to the end; the last interrupt must replay to the end.
Backtracking is what handles two interrupts close together, where the effect of
the first shows only after the second.

On the example firmware, `rewind hunt --variant unguarded --no-sites` records a
failure the way a part would. Replay alone diverges at the first write that
depends on the lost update. `rewind locate` finds the one interrupt that landed
inside the window, places the other twenty-three right after their crossings,
and reproduces the recording exactly: the same checksum and state hash as the
run that kept its sites (`tests/test_unguarded.cpp`). `rewind landings
--threshold 0` independently names the same interrupt, from the clock alone.

What it does not do:

- **It cannot tell apart what the trace cannot.** A placement that changes
  nothing a register read or a recorded write ever shows is indistinguishable
  from the real one. After finding a solution, each interrupt is tried at its
  other placements with everything else held; any that also reproduces the
  whole trace makes it *ambiguous*, and the earliest is kept. Two interrupts
  close together can swap without the trace noticing, so those are often
  ambiguous, and that is the right answer. It also needs writes to be
  recorded: with `--no-writes` the oracle is the reads alone.
- **It can only choose among the probe points the host build has.** If the real
  landing was somewhere with none, no placement fits, the first such interrupt
  is reported unresolved and the replay still diverges. The fix is a probe
  point there; `rewind landings` says where to look.
- **It is a search over replays of the whole trace.** The cost is the number of
  runs times the length of the trace: about fifty replays of six thousand events
  here, nothing. A long recording would want to resume from checkpoints, as
  reverse execution does; it does not yet. `max_runs` stops a search that gets
  out of hand and says so.

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
is invisible to this model unless a probe point is placed in the window (see
*Probe points* below). Making every basic block a crossing would need
instrumentation that costs far more than 2% and is a different project.

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

## DMA

A DMA controller writes RAM with no CPU involved. Nothing on a part observes
the deposit: the shim sits between the firmware and the hardware, and a
controller is hardware that never goes through it.

There were two ways to record it. One is to record the *deposit*: an event when
the data lands, applied to the firmware's memory at the right point in replay.
That needs two things the device cannot give: to know when it landed, and to
know what landed without copying the buffer on every access. It would also
bring back the problem the interrupt landing has, because a deposit is
asynchronous and replay can only apply it at a crossing.

The other is to record the *look*. The firmware reads a DMA buffer through the
shim (`hal_dma_read`), and each read is recorded with the bytes it saw. What the
firmware sees is a function of the trace and nothing else, which is the
property replay needs; when the DMA wrote it does not matter, only what was
there when the firmware looked. It is an access like any register read, so the
position is exact, and there is nothing to search for. `EV_DMA` is that record:
a buffer id, an offset, and up to 16 bytes, with longer reads split the same
way on record and replay (the block buffer is caller-supplied and may be only 64
bytes, so a larger event could not be promised to fit).

The cost is a rule for the firmware: DMA memory is read through the shim, not
dereferenced. A direct read is invisible to the recorder, and in replay it sees
whatever the host buffer holds; nothing can detect that, so it is documented
rather than checked. It is the same rule registers already have, and for the
same reason.

The buffer is named by id, because a device address means nothing on the host.
The firmware registers the same ids on both; on a part,
`HardwareHal::add_dma_region` records where each lives, and a read of an
unregistered region or past its end gives zeros, which the trace then records
like any other value.

Replay checks a DMA read against the trace as it checks an access: region,
offset and length must match the next event, or the run diverges. The
simulator's DMA model (`Mcu::enable_dma`) is a ring written whenever a byte
arrives, with a count register the firmware polls; it is lapped if the firmware
is slow, and `tests/test_dma.cpp` records such an overrun and replays it,
losing the same bytes.

## Reverse execution

Covered in its own document -- see `reverse-execution.md`. The short version:
replay is deterministic, so moving backwards is re-executing to an earlier
point, and checkpoints make it affordable. Because bare-metal state is small
and explicit, a checkpoint is a struct copy rather than a `fork()`.

The one constraint it places on this model is where checkpoints may be taken:
between main-loop iterations, where no call is in progress and the interrupt
mask is balanced. Those are the points at which a data-only snapshot is a
complete description of the run.

## State that is not recorded

The firmware's RAM is not in the trace. Replay reconstructs it by
re-executing, which is what makes traces small — kilobytes, not megabytes.

The consequence is that replay must start from the same initial state as the
recording. Right now that means starting from reset. Recording from an
arbitrary point would need an initial RAM snapshot in the header, which is
straightforward and not yet done.
