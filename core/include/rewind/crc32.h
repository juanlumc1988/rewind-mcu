// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// CRC-32/ISO-HDLC (the zlib/Ethernet one: poly 0xEDB88320 reflected,
// init 0xFFFFFFFF, final xor 0xFFFFFFFF).
//
// Implemented nibble-wise on purpose. The byte-wise variant needs a 1 KiB
// table; this one needs 64 bytes of .rodata at roughly half the throughput.
// On a part where flash is the scarce resource and the CRC only ever runs
// over a block that is about to be pushed out a UART anyway, 960 bytes of
// flash is the better trade.

#ifndef REWIND_CRC32_H
#define REWIND_CRC32_H

#include "rewind/rw_types.h"

namespace rwd {

// Running-state API, for checksumming a block that arrives in pieces.
u32 crc32_init();
u32 crc32_update(u32 crc, const u8* data, u32 len);
u32 crc32_final(u32 crc);

// One-shot convenience wrapper.
u32 crc32(const u8* data, u32 len);

} // namespace rwd

#endif // REWIND_CRC32_H
