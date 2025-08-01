// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_CXX20_IS_CONSTANT_EVALUATED_H_
#define BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_CXX20_IS_CONSTANT_EVALUATED_H_

namespace partition_alloc::internal::base {

// std::is_constant_evaluated was introduced in C++20. PartitionAlloc's minimum
// supported C++ version is C++17.
#if defined(__cpp_lib_is_constant_evaluated) && \
    __cpp_lib_is_constant_evaluated >= 201811L

#include <type_traits>
using std::is_constant_evaluated;

#else

#if defined(MOZ_ZUCCHINI)
#include "base/compiler_specific.h"
#endif  // defined(MOZ_ZUCCHINI)

// Implementation of C++20's std::is_constant_evaluated.
//
// References:
// - https://en.cppreference.com/w/cpp/types/is_constant_evaluated
// - https://wg21.link/meta.const.eval
constexpr bool is_constant_evaluated() noexcept {
#if !defined(MOZ_ZUCCHINI) || HAS_BUILTIN(__builtin_is_constant_evaluated)
  return __builtin_is_constant_evaluated();
#else
  return false;
#endif  // !defined(MOZ_ZUCCHINI) || HAS_BUILTIN(__builtin_is_constant_evaluated)
}

#endif

}  // namespace partition_alloc::internal::base

#endif  // BASE_ALLOCATOR_PARTITION_ALLOCATOR_PARTITION_ALLOC_BASE_CXX20_IS_CONSTANT_EVALUATED_H_
