// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/firmware.h"

namespace rwhost {

u64 Firmware::state_hash() const {
    std::vector<u8> bytes(state_size());
    if (!bytes.empty()) {
        save_state(bytes.data());
    }
    // FNV-1a, 64 bit.
    u64 h = 14695981039346656037ULL;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        h ^= (u64)bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool Firmware::restore_region(u32 region, const u8* data, std::size_t len) {
    if (region != 0 || len != state_size()) {
        return false;
    }
    restore_state(data);
    return true;
}

} // namespace rwhost
