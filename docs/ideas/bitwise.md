# bitwise — automatic protocol reverse engineering

Parked idea, kept here so it does not evaporate. Not started.

## The problem

You have a device on a bus — CAN, RS-485, SPI, a UART link to a module whose
vendor went out of business — and no documentation. The current workflow is a
logic analyser, a spreadsheet, and a week of staring at hex.

## The idea

Feed it *N* captures and have it infer the structure, rather than reading it
off by eye.

**Frame segmentation.** Inter-byte gap timing gives frame boundaries almost
for free on an idle-delimited bus. Where it does not, cluster candidate
lengths and score them by how much structure each segmentation exposes.

**Field discovery.** Line the frames up and compute per-bit-offset entropy
across the corpus. Constant bits are magic numbers, headers or padding.
High-entropy runs are payload. Everything in between is a field boundary,
and boundaries show up as discontinuities in the entropy profile.

**Field typing.** Once you have candidate fields, classify them by behaviour
over the corpus:
- constant delta between consecutive frames → sequence counter (and the
  delta tells you the increment)
- monotonic with a delta proportional to inter-frame time → timestamp
- correlates with frame length → a length field
- small cardinality → an enum or a message type discriminator
- fits none of the above and is uniformly distributed → payload

**Checksum identification.** The part worth building for its own sake. Given
a set of frames and a candidate checksum field, search the CRC parameter
space — polynomial, initial value, input reflection, output reflection, final
xor, and which byte range is covered — for a parameterisation that validates
every frame in the corpus. The catalogued CRC-8/16/32 variants are a few
hundred combinations and fall out instantly; an uncatalogued polynomial needs
a real search, and there is a neat trick available, since for a fixed
reflection and width the polynomial can be recovered algebraically from a
handful of frames that differ in one byte rather than brute-forced.

**Output.** A generated C++ header, or a Kaitai Struct definition, plus a
Wireshark-style Qt view over the capture with fields coloured by inferred
type.

## Why it is worth doing

Every embedded engineer has reverse engineered a protocol by hand, and the
process is mechanical enough to automate but fiddly enough that nobody does.
The CRC search alone would save days, and unlike the rest of it, the answer
is verifiable: either the parameters validate every frame or they do not.

## Relationship to rewind

Independent, but they share a spine. `rewind` records what crossed the HAL
boundary; `bitwise` infers structure in what crossed a wire. The trace format
in `core/` would serve as a capture container with little change, and the
frame-segmentation stage is the same problem as reconstructing peripheral
activity from a trace.
