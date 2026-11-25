# Changelog

Versions follow [semantic versioning](https://semver.org). What 1.0 does and does
not claim is in the [README](README.md#what-this-does-not-do-yet): the host side
is complete and tested; the device side has been cross-compiled and run on an
emulated Cortex-M4, and never on a part.

## [Unreleased]

### Added

- **Executables with each release**: for Linux x86-64, `rewind` as one static
  executable and `rewind-gui` (which needs Qt 6); for Windows x86-64,
  `rewind.exe`, cross-compiled with MinGW as one static executable. Built by
  `binaries.yml` on every change, with the Windows unit suite run under Wine and
  its traces compared byte for byte with Linux's; attached to a release by
  `release.yml` when it is published, or on demand for a release that already
  exists. No change to the library or to the trace format.

## [1.0.0]

The first release. What is new in it, and then what it is made of.

### Added in 1.0

- **A firmware of your own.** `rwhost::Firmware` is the handful of operations the
  host tools need of a firmware (start it, run iterations of its main loop, save
  and restore its state, optionally describe it). Replay (`replay_firmware`),
  recording against a model of the hardware (`record_firmware`), the timeline,
  the landing search (`locate_firmware`), `TraceDoc`, `diff_fields` and the
  viewer all work through it; the example firmware is one implementation
  (`ExampleFirmware`). `examples/custom-firmware` is a thermostat that uses it.
- **An installable package.** `cmake --install` and `find_package(rewind 1)` give
  `rewind::core`, `rewind::target`, `rewind::sim`, `rewind::fw`, `rewind::host`
  and, if installed with the viewer, `rewind::gui`. Any 1.x is accepted and any
  other major is refused. An install from the cross-compiled build carries only
  what runs on the part.
- **Recordings that start in the middle of a run.** Trace format 6 adds
  `EV_RAM`, an image of the firmware's declared state ahead of the first event
  (`hal_state_region`, `hal_snapshot`). Replay, the timeline and the landing
  search start from it. `rewind record --skip N`.
- **Interrupts that preempt each other, by priority.** `hal_set_irq_priority`,
  `HalOps::poll_irq_pri`; the simulator gets a timer as a second source. A
  backend that does not provide it, and any firmware that sets no priorities,
  behave exactly as before.
- **A second clock for CYCCNT's wraps.** `ClockExtender::extend(raw, ref)` and
  `HardwareHal::set_reference_clock` count the periods a long silence hid, which
  the counter alone cannot.
- `rewind --version` and `rewind-gui --version`; the version is generated from
  one place (`rewind/version.h`).
- The viewer's state panel shows whatever the firmware describes, with array
  elements gathered into a row and the state's hash below.

### Changed in 1.0

- The trace format is version 6. The reader still reads 2 to 6; a reader from
  before 1.0 refuses a version 6 trace. Traces that do not begin mid-run differ
  from version 5 only in the number in the header.
- `Snapshot` (the timeline's) carries the state as bytes and as named fields.
  `Snapshot::fw`, the example's typed view, stays and is filled only for the
  example.
- The target-side libraries grew: roughly 4.6 KB of flash to record, from 3.7 KB
  (README, [What it costs in flash](README.md#what-it-costs-in-flash)).

### What it is made of

Before 1.0 the pieces were added in stages; they are all part of this release.

- Record, replay and reverse execution: a block-framed, CRC-checked trace format
  that survives being cut short and says when blocks were lost; the shim and its
  three modes; the recorder, ring buffer and drain; cycle-counter extension; a
  checkpointed timeline with exact seeks and bisection (`rewind inspect`).
- Interrupts where hardware put them: return addresses (`EV_IRQ_AT`), probe
  points (`RWD_SITE`, `EV_SITE`), `rewind landings`, and `rewind locate`, a
  search for where an interrupt landed in a trace recorded without probe points.
- A Cortex-M interrupt trampoline (`RWD_CORTEX_M_ISR`), cross-compiled on every
  push and run on an emulated Cortex-M4 in CI.
- DMA (`EV_DMA`, `hal_dma_read`).
- Trace comparison (`rewind diff`) and a Qt viewer (`rewind-gui`) with a timeline
  per trace, the events as a list, the firmware's state, and the places two runs
  part.
- CI on GCC and Clang, under ASan and UBSan, cross-compiled to a Cortex-M4 with
  a check that the target side pulls in no heap, exceptions or RTTI, under QEMU,
  with the viewer, and against the installed package.

### Known limits

Unchanged by 1.0, and stated in the README: nothing has run on silicon; finer
checkpoints would need the call stack captured; there is no transport (SWO, RTT,
UART) and no measured CPU overhead; an interrupt that lands mid-computation has
no replayable point unless a probe point is there; and a memory image is only as
complete as the firmware's declaration of its state.
