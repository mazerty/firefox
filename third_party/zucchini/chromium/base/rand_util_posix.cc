// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/rand_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "base/check.h"
#include "base/compiler_specific.h"
#if !defined(MOZ_ZUCCHINI)
#include "base/feature_list.h"
#endif  // !defined(MOZ_ZUCCHINI)
#include "base/files/file_util.h"
#if !defined(MOZ_ZUCCHINI)
#include "base/metrics/histogram_macros.h"
#endif  // !defined(MOZ_ZUCCHINI)
#include "base/no_destructor.h"
#include "base/posix/eintr_wrapper.h"
#include "base/time/time.h"
#include "build/build_config.h"

#if (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)) && !BUILDFLAG(IS_NACL)
#if !defined(MOZ_ZUCCHINI)
#include "third_party/lss/linux_syscall_support.h"
#endif  // !defined(MOZ_ZUCCHINI)
#elif BUILDFLAG(IS_MAC)
// TODO(crbug.com/995996): Waiting for this header to appear in the iOS SDK.
// (See below.)
#include <sys/random.h>
#endif

#if !defined(MOZ_ZUCCHINI)
#if !BUILDFLAG(IS_NACL)
#include "third_party/boringssl/src/include/openssl/crypto.h"
#include "third_party/boringssl/src/include/openssl/rand.h"
#endif
#endif  // !defined(MOZ_ZUCCHINI)

namespace base {

namespace {

#if BUILDFLAG(IS_AIX)
// AIX has no 64-bit support for O_CLOEXEC.
static constexpr int kOpenFlags = O_RDONLY;
#else
static constexpr int kOpenFlags = O_RDONLY | O_CLOEXEC;
#endif

// We keep the file descriptor for /dev/urandom around so we don't need to
// reopen it (which is expensive), and since we may not even be able to reopen
// it if we are later put in a sandbox. This class wraps the file descriptor so
// we can use a static-local variable to handle opening it on the first access.
class URandomFd {
 public:
  URandomFd() : fd_(HANDLE_EINTR(open("/dev/urandom", kOpenFlags))) {
    CHECK(fd_ >= 0) << "Cannot open /dev/urandom";
  }

  ~URandomFd() { close(fd_); }

  int fd() const { return fd_; }

 private:
  const int fd_;
};

#if !defined(MOZ_ZUCCHINI)
#if (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS) || \
     BUILDFLAG(IS_ANDROID)) &&                        \
    !BUILDFLAG(IS_NACL)
// TODO(pasko): Unify reading kernel version numbers in:
// mojo/core/channel_linux.cc
// chrome/browser/android/seccomp_support_detector.cc
void KernelVersionNumbers(int32_t* major_version,
                          int32_t* minor_version,
                          int32_t* bugfix_version) {
  struct utsname info;
  if (uname(&info) < 0) {
    NOTREACHED();
    *major_version = 0;
    *minor_version = 0;
    *bugfix_version = 0;
    return;
  }
  int num_read = sscanf(info.release, "%d.%d.%d", major_version, minor_version,
                        bugfix_version);
  if (num_read < 1)
    *major_version = 0;
  if (num_read < 2)
    *minor_version = 0;
  if (num_read < 3)
    *bugfix_version = 0;
}

bool KernelSupportsGetRandom() {
  int32_t major = 0;
  int32_t minor = 0;
  int32_t bugfix = 0;
  KernelVersionNumbers(&major, &minor, &bugfix);
  if (major > 3 || (major == 3 && minor >= 17))
    return true;
  return false;
}

bool GetRandomSyscall(void* output, size_t output_length) {
  // We have to call `getrandom` via Linux Syscall Support, rather than through
  // the libc wrapper, because we might not have an up-to-date libc (e.g. on
  // some bots).
  const ssize_t r =
      HANDLE_EINTR(syscall(__NR_getrandom, output, output_length, 0));

  // Return success only on total success. In case errno == ENOSYS (or any other
  // error), we'll fall through to reading from urandom below.
  if (output_length == static_cast<size_t>(r)) {
    MSAN_UNPOISON(output, output_length);
    return true;
  }
  return false;
}
#endif  // (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS) ||
        // BUILDFLAG(IS_ANDROID)) && !BUILDFLAG(IS_NACL)

#if BUILDFLAG(IS_ANDROID)
std::atomic<bool> g_use_getrandom;

// Note: the BoringSSL feature takes precedence over the getrandom() trial if
// both are enabled.
BASE_FEATURE(kUseGetrandomForRandBytes,
             "UseGetrandomForRandBytes",
             FEATURE_ENABLED_BY_DEFAULT);

bool UseGetrandom() {
  return g_use_getrandom.load(std::memory_order_relaxed);
}
#elif (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)) && !BUILDFLAG(IS_NACL)
bool UseGetrandom() {
  return true;
}
#endif
#endif  // !defined(MOZ_ZUCCHINI)

}  // namespace

#if !defined(MOZ_ZUCCHINI)
namespace internal {

#if BUILDFLAG(IS_ANDROID)
void ConfigureRandBytesFieldTrial() {
  g_use_getrandom.store(FeatureList::IsEnabled(kUseGetrandomForRandBytes),
                        std::memory_order_relaxed);
}
#endif

namespace {

#if !BUILDFLAG(IS_NACL)
// The BoringSSl helpers are duplicated in rand_util_fuchsia.cc and
// rand_util_win.cc.
std::atomic<bool> g_use_boringssl;

BASE_FEATURE(kUseBoringSSLForRandBytes,
             "UseBoringSSLForRandBytes",
             FEATURE_DISABLED_BY_DEFAULT);

}  // namespace

void ConfigureBoringSSLBackedRandBytesFieldTrial() {
  g_use_boringssl.store(FeatureList::IsEnabled(kUseBoringSSLForRandBytes),
                        std::memory_order_relaxed);
}

bool UseBoringSSLForRandBytes() {
  return g_use_boringssl.load(std::memory_order_relaxed);
}
#endif

}  // namespace internal
#endif  // !defined(MOZ_ZUCCHINI)

namespace {

void RandBytes(void* output, size_t output_length, bool avoid_allocation) {
#if !defined(MOZ_ZUCCHINI)
#if !BUILDFLAG(IS_NACL)
  // The BoringSSL experiment takes priority over everything else.
  if (!avoid_allocation && internal::UseBoringSSLForRandBytes()) {
    // Ensure BoringSSL is initialized so it can use things like RDRAND.
    CRYPTO_library_init();
    // BoringSSL's RAND_bytes always returns 1. Any error aborts the program.
    (void)RAND_bytes(static_cast<uint8_t*>(output), output_length);
    return;
  }
#endif
#endif  // !defined(MOZ_ZUCCHINI)
#if (BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS) || \
     BUILDFLAG(IS_ANDROID)) &&                        \
    !BUILDFLAG(IS_NACL)
#if !defined(MOZ_ZUCCHINI)
  if (avoid_allocation || UseGetrandom()) {
    // On Android it is mandatory to check that the kernel _version_ has the
    // support for a syscall before calling. The same check is made on Linux and
    // ChromeOS to avoid making a syscall that predictably returns ENOSYS.
    static const bool kernel_has_support = KernelSupportsGetRandom();
    if (kernel_has_support && GetRandomSyscall(output, output_length))
      return;
  }
#endif  // !defined(MOZ_ZUCCHINI)
#elif BUILDFLAG(IS_MAC)
  // TODO(crbug.com/995996): Enable this on iOS too, when sys/random.h arrives
  // in its SDK.
  if (getentropy(output, output_length) == 0) {
    return;
  }
#endif

  // If the OS-specific mechanisms didn't work, fall through to reading from
  // urandom.
  //
  // TODO(crbug.com/995996): When we no longer need to support old Linux
  // kernels, we can get rid of this /dev/urandom branch altogether.
  const int urandom_fd = GetUrandomFD();
  const bool success =
      ReadFromFD(urandom_fd, static_cast<char*>(output), output_length);
  CHECK(success);
}

}  // namespace

namespace internal {

double RandDoubleAvoidAllocation() {
  uint64_t number;
  RandBytes(&number, sizeof(number), /*avoid_allocation=*/true);
  // This transformation is explained in rand_util.cc.
  return (number >> 11) * 0x1.0p-53;
}

}  // namespace internal

void RandBytes(void* output, size_t output_length) {
  RandBytes(output, output_length, /*avoid_allocation=*/false);
}

int GetUrandomFD() {
  static NoDestructor<URandomFd> urandom_fd;
  return urandom_fd->fd();
}

}  // namespace base
