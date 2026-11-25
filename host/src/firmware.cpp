// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "rewind/firmware.h"

#include "rewind/replay.h"

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

bool start_firmware(Firmware& firmware, const ReplayHal& trace, std::string* why) {
    firmware.reset();
    firmware.install();

    const std::map<u32, std::vector<u8> > image = trace.image();
    for (std::map<u32, std::vector<u8> >::const_iterator it = image.begin(); it != image.end(); ++it) {
        if (!firmware.restore_region(it->first, it->second.data(), it->second.size())) {
            if (why != 0) {
                *why = "the firmware cannot take the memory image the recording started from: "
                       "region " + std::to_string(it->first) + ", " +
                       std::to_string(it->second.size()) + " bytes";
            }
            return false;
        }
    }
    return true;
}

} // namespace rwhost
