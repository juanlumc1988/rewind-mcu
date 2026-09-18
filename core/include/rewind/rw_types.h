// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// Fixed-width integer types and a compile-time assertion, both written for
// C++98. <cstdint> is a C++11 header and <stdint.h> is C99, so neither is
// guaranteed on the ancient cross-toolchains this code must survive.
// The static assertions below are what actually pin the widths down: if a
// target disagrees, the build fails here rather than silently corrupting
// every trace it writes.

// The namespace is `rwd`, not `rewind`, and that is not a typo. C89 declares
// void rewind(FILE*), which <cstdio> drops into the global scope, so a
// namespace of that name collides the moment any translation unit includes a
// standard header before ours. The project keeps its name; the namespace
// takes the file extension instead.
//
#ifndef REWIND_RW_TYPES_H
#define REWIND_RW_TYPES_H

namespace rwd {

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef signed long long   i64;

// C++98 has no static_assert. Instantiating StaticAssert<false> has no
// member Type, so the typedef below is a hard compile error.
template <bool B> struct StaticAssert;
template <> struct StaticAssert<true> { typedef int Type; };

} // namespace rwd

// `tag` must be a unique identifier within its scope.
#define RW_STATIC_ASSERT(cond, tag) \
    typedef ::rwd::StaticAssert<(cond)>::Type rw_static_assert_##tag

namespace rwd {

RW_STATIC_ASSERT(sizeof(u8)  == 1, u8_is_one_byte);
RW_STATIC_ASSERT(sizeof(u16) == 2, u16_is_two_bytes);
RW_STATIC_ASSERT(sizeof(u32) == 4, u32_is_four_bytes);
RW_STATIC_ASSERT(sizeof(u64) == 8, u64_is_eight_bytes);
RW_STATIC_ASSERT(sizeof(i32) == 4, i32_is_four_bytes);
RW_STATIC_ASSERT(sizeof(i64) == 8, i64_is_eight_bytes);

} // namespace rwd

#endif // REWIND_RW_TYPES_H
