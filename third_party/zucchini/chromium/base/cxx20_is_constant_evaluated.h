// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_CXX20_IS_CONSTANT_EVALUATED_H_
#define BASE_CXX20_IS_CONSTANT_EVALUATED_H_

#if defined(MOZ_ZUCCHINI)
#include "base/compiler_specific.h"
#endif  // defined(MOZ_ZUCCHINI)

namespace base {

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

}  // namespace base

#endif  // BASE_CXX20_IS_CONSTANT_EVALUATED_H_
