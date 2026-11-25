// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "doctest.h"

#include <cstdio>
#include <string>

#include "rewind/trace.h"
#include "rewind/version.h"

TEST_CASE("the version is one number with three parts") {
    // The string and the three numbers are generated from the same place; they
    // must say the same thing, or a tool and a build script would disagree.
    char expected[32];
    std::snprintf(expected, sizeof(expected), "%d.%d.%d", REWIND_VERSION_MAJOR,
                  REWIND_VERSION_MINOR, REWIND_VERSION_PATCH);
    CHECK(std::string(REWIND_VERSION) == expected);

    CHECK(REWIND_VERSION_MAJOR >= 1);
    CHECK(REWIND_VERSION_MINOR >= 0);
    CHECK(REWIND_VERSION_PATCH >= 0);
}

TEST_CASE("the trace format a build writes is one it also reads") {
    CHECK(rwd::kTraceMinVersion <= rwd::kTraceVersion);
}
