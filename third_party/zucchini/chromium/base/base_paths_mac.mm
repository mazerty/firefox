// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Defines base::PathProviderMac which replaces base::PathProviderPosix for Mac
// in base/path_service.cc.

#import <Foundation/Foundation.h>

#include "base/apple/bundle_locations.h"
#include "base/base_paths.h"
#include "base/base_paths_apple.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/mac/foundation_util.h"
#include "base/notreached.h"
#include "base/path_service.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace base {

bool PathProviderMac(int key, base::FilePath* result) {
  switch (key) {
    case base::FILE_EXE:
      *result = base::apple::internal::GetExecutablePath();
      return true;
    case base::FILE_MODULE:
      return base::apple::internal::GetModulePathForAddress(
          result, reinterpret_cast<const void*>(&base::PathProviderMac));
    case base::DIR_APP_DATA: {
      bool success =
          base::mac::GetUserDirectory(NSApplicationSupportDirectory, result);
      return success;
    }
    case base::DIR_SRC_TEST_DATA_ROOT:
      // Go through PathService to catch overrides.
      if (!PathService::Get(base::FILE_EXE, result)) {
        return false;
      }

      // Start with the executable's directory.
      *result = result->DirName();

#if !defined(MOZ_ZUCCHINI)
      if (base::mac::AmIBundled()) {
        // The bundled app executables (Chromium, TestShell, etc) live five
        // levels down, eg:
        // src/xcodebuild/{Debug|Release}/Chromium.app/Contents/MacOS/Chromium
        *result = result->DirName().DirName().DirName().DirName().DirName();
      } else {
        // Unit tests execute two levels deep from the source root, eg:
        // src/xcodebuild/{Debug|Release}/base_unittests
        *result = result->DirName().DirName();
      }
#else
      // Firefox unit tests execute three levels deep from the source root, eg:
      // ./obj-aarch64-apple-darwin24.5.0/dist/bin/zucchini-gtest
      *result = result->DirName().DirName().DirName();
#endif  // !defined(MOZ_ZUCCHINI)
      return true;
#if !defined(MOZ_ZUCCHINI)
    case base::DIR_USER_DESKTOP:
      return base::mac::GetUserDirectory(NSDesktopDirectory, result);
    case base::DIR_ASSETS:
      if (!base::mac::AmIBundled()) {
        return PathService::Get(base::DIR_MODULE, result);
      }
      *result = base::apple::FrameworkBundlePath().Append(
          FILE_PATH_LITERAL("Resources"));
      return true;
    case base::DIR_CACHE:
      return base::mac::GetUserDirectory(NSCachesDirectory, result);
#endif  // !defined(MOZ_ZUCCHINI)
    default:
      return false;
  }
}

}  // namespace base
