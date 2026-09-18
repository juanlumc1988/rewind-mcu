# Reverse execution

Moving to any point in a recorded run, forwards or backwards, and reading the
firmware's state there.

## The claim

Replay is deterministic: the state at event N is a pure function of the trace
and N. So there is nothing to undo. "Going back" is re-executing to an
earlier point, and the only question is how to make that cheap.

Naively it costs O(N) per query, so walking back through a run one event at a
time costs O(N²). Checkpoints fix it: snapshot periodically, and a seek
restarts from the nearest checkpoint at or before the target.

## Why this is easier here than in rr

`rr` and UndoDB checkpoint with `fork()`. They have to: the state of a
general-purpose Linux process is large, opaque, and full of things you cannot
copy by hand — file descriptors, memory mappings, thread stacks, kernel-side
bookkeeping. Copy-on-write is the only affordable snapshot.

Bare-metal firmware has none of that. Its entire state is:

- the firmware's static storage (`fw::State`, a few dozen bytes),
- the replay cursor and timestamp,
- the shim's own scalars — mode, mask depth, ISR table.

No heap, no dynamic objects, nothing hidden behind an abstraction. A
checkpoint is a struct copy, which is faster than `fork()`, works off Linux,
and can be inspected in a debugger.

This is worth stating plainly because it looks like a shortcut and is not.
The property that makes reverse execution expensive in userspace — unbounded,
opaque process state — is precisely the property bare-metal firmware lacks by
construction. The technique gets *easier* as the target gets smaller, which
is the opposite of how these things usually go.

## What a struct copy cannot capture

The call stack. A snapshot says what the data is, not where execution was.

So checkpoints are taken between main-loop iterations, where there is no call
in progress and the interrupt mask is balanced — the two conditions that make
a data-only description complete. Everything needed to resume is then the
data plus one integer, the iteration number.

Landing on an exact event is a separate problem from checkpointing, and is
solved separately: replay forward from the nearest checkpoint with a capture
point armed, and the `ReplayHal` fires a callback at the instant that event
is about to be served.

### Before, not after

The capture fires immediately *before* the target event is served, with the
firmware still in the state that preceded it. This is a deliberate choice.
The question worth asking of a trace is "what did the firmware look like when
that interrupt arrived?", and the answer wants the state before it was
handled, not after.

There is exactly one first point at which the cursor sits on a given event —
whichever access asks for it first, a read, a write or an interrupt poll — so
the capture is deterministic.

## What was considered and rejected

**`longjmp` out of the firmware** to stop exactly on an event. It works, and
the frames it would unwind are all POD, so it is safe here. But it is
unnecessary: running to the end of the iteration that contains the target
costs a handful of events, and the capture callback has already recorded the
answer. Not worth the hazard for the saving.

**Letting the run finish and capturing in passing**, with no early exit at
all. Simplest of the three, and it makes every seek O(N) again — which
defeats the checkpoints entirely.

**`fork()`-based checkpoints**, as the roadmap originally said. Rejected once
it was clear how small the state actually is: a fork per checkpoint costs
more than the snapshot it replaces, and drags in process management and a
Linux dependency for nothing.

## Searching

`Timeline::find_first` bisects for the first point at which a predicate
holds. Asking when a counter reached a value goes from a linear scan of the
run to a handful of seeks: on the example trace, fifteen seeks over six
thousand events.

The predicate must be monotonic over the run — false up to a point, true from
there on. Most things worth asking about firmware are: bytes consumed, a flag
that latches, a high-water mark. A predicate that flickers returns *a* point
where it holds, not the first, and the header says so.

## Cost

Measured rather than asserted; `--checkpoints` sets the interval so the
difference can be watched directly. Ten backward steps on the example trace:

| Checkpoint interval | Events replayed |
|---|---|
| every 128 iterations | 7 150 |
| effectively none | 29 062 |

Checkpoints reduce the cost of each seek, not the number of seeks — which is
why a bisection takes the same fifteen seeks either way, each one simply
cheaper.

## What the tests pin down

The golden property is that a checkpoint is a *complete* description. So
reaching an event from a nearby checkpoint must be indistinguishable from
re-executing the whole trace to get there — `tests/test_timeline.cpp` runs
both and compares, at forty points across the run.

The rest follows from it: seeks are repeatable, a backward walk matches
direct seeks at every step, forward-then-back returns to the same place, and
neither work done nor the clock ever increases while moving backwards.
