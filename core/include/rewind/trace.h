// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// On-disk trace format.
//
//   Header (32 bytes, little-endian)
//     +0  u32  magic      'R','W','N','D' as stored bytes
//     +4  u16  version
//     +6  u16  flags
//     +8  u64  build_id   identifies the firmware build; replaying a trace
//                         against a different build is the single easiest
//                         way to chase a ghost, so we refuse it.
//    +16  u64  seed       whatever produced the inputs (simulator seed,
//                         unit serial number, boot counter...). Informational.
//    +24  u64  reserved   zero
//
//   Blocks, repeated until a terminator
//     u32  payload_len    little-endian; 0 terminates the stream
//     u32  seq            little-endian; counts from 0, never reused
//     u8   payload[payload_len]
//     u32  crc32          little-endian, over seq and payload
//
// Blocking exists so a trace stays usable when it is cut short: a device
// that browns out mid-transmission leaves whole, CRC-checked blocks behind
// and one ragged tail, and the reader stops cleanly at the tail rather than
// reporting garbage.
//
// `seq` is what makes a LOST block different from a corrupt one, and the
// distinction is the whole reason it is here. A device recording faster than
// it can drain its buffer drops whole blocks. Without a sequence number the
// trace still parses -- every surviving block has a valid CRC -- and replay
// then diverges somewhere downstream, for reasons that look like a firmware
// bug and are not. With it, the reader sees the gap immediately and says so.
// Four bytes per block, against a class of failure that would otherwise cost
// an afternoon.
//
// Events inside a payload, each a type byte followed by varints:
//   EV_MMIO_READ   delta_ts, delta_addr, value
//   EV_MMIO_WRITE  delta_ts, delta_addr, value
//   EV_IRQ_ENTER   delta_ts, vector
//   EV_STOP        delta_ts
//
// Two fields are deltas against the previous event, across the whole stream
// rather than per block, and both earn their keep:
//
//   delta_ts     Consecutive accesses are a handful of cycles apart, so this
//                is nearly always one byte where an absolute timestamp would
//                be five or more.
//
//   delta_addr   ZigZag-coded, because it goes both ways. Peripheral
//                addresses live high in the map -- 0x40000000 costs five
//                bytes in LEB128, every single time -- but firmware pokes
//                the same handful of registers over and over, so the delta
//                is zero or a few words and costs one. This one change takes
//                roughly four bytes off every MMIO event, which on a device
//                shipping traces over a 115200 baud UART is the difference
//                between viable and not. EV_IRQ_ENTER's argument is a vector
//                number, not an address, so it is coded plainly and leaves
//                the address predictor alone.

#ifndef REWIND_TRACE_H
#define REWIND_TRACE_H

#include "rewind/rw_types.h"
#include "rewind/varint.h"

namespace rwd {

// Stored little-endian, so byte 0 is 'R' (0x52) and byte 3 is 'D' (0x44).
const u32 kTraceMagic      = 0x444E5752u;
const u16 kTraceVersion    = 2;
const u32 kTraceHeaderSize = 32;

// Per-block framing overhead: payload_len + seq + crc32.
const u32 kBlockOverhead = 12;

enum EventType {
    EV_MMIO_READ  = 0x01,
    EV_MMIO_WRITE = 0x02,
    EV_IRQ_ENTER  = 0x03,
    EV_STOP       = 0x04
};

enum TraceFlags {
    // Writes are firmware *output*: given identical inputs they are implied,
    // so recording them is redundant. We record them anyway by default
    // because during replay they are exactly what proves the replay did not
    // silently diverge. Clear this flag to halve a trace on a tight link.
    kFlagRecordWrites = 0x0001
};

// 1 type byte + 3 varints at worst.
const u32 kMaxEventBytes = 1 + 3 * kVarintMaxBytes;

struct TraceHeader {
    u32 magic;
    u16 version;
    u16 flags;
    u64 build_id;
    u64 seed;
};

struct Event {
    u8  type;
    u64 ts;
    u32 addr;   // EV_MMIO_*: address.  EV_IRQ_ENTER: vector number.
    u32 value;  // EV_MMIO_*: value.    Otherwise zero.
};

// -- little-endian byte access, independent of host endianness --

inline void put_u16_le(u8* p, u16 v) {
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
}

inline void put_u32_le(u8* p, u32 v) {
    p[0] = (u8)(v & 0xFFu);
    p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu);
    p[3] = (u8)((v >> 24) & 0xFFu);
}

inline void put_u64_le(u8* p, u64 v) {
    put_u32_le(p, (u32)(v & 0xFFFFFFFFu));
    put_u32_le(p + 4, (u32)((v >> 32) & 0xFFFFFFFFu));
}

inline u16 get_u16_le(const u8* p) {
    return (u16)((u16)p[0] | (u16)((u16)p[1] << 8));
}

inline u32 get_u32_le(const u8* p) {
    return (u32)p[0]
         | ((u32)p[1] << 8)
         | ((u32)p[2] << 16)
         | ((u32)p[3] << 24);
}

inline u64 get_u64_le(const u8* p) {
    return (u64)get_u32_le(p) | ((u64)get_u32_le(p + 4) << 32);
}

} // namespace rwd

#endif // REWIND_TRACE_H
