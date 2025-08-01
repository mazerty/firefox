// Copyright 2011 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sandbox/win/src/win_utils.h"

#include <windows.h>

#include <ntstatus.h>
#include <psapi.h>
#include <stddef.h>
#include <stdint.h>

#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/numerics/safe_math.h"
#include "base/strings/string_util.h"
#include "base/win/pe_image.h"
#include "base/win/scoped_handle.h"
#include "base/win/win_util.h"
#include "sandbox/win/src/internal_types.h"
#include "sandbox/win/src/nt_internals.h"
#include "sandbox/win/src/sandbox_nt_util.h"

namespace {

const size_t kDriveLetterLen = 3;

constexpr wchar_t kNTDotPrefix[] = L"\\\\.\\";
const size_t kNTDotPrefixLen = std::size(kNTDotPrefix) - 1;

// Holds the information about a known registry key.
struct KnownReservedKey {
  const wchar_t* name;
  HKEY key;
};

// Contains all the known registry key by name and by handle.
const KnownReservedKey kKnownKey[] = {
    {L"HKEY_CLASSES_ROOT", HKEY_CLASSES_ROOT},
    {L"HKEY_CURRENT_USER", HKEY_CURRENT_USER},
    {L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE},
    {L"HKEY_USERS", HKEY_USERS},
    {L"HKEY_PERFORMANCE_DATA", HKEY_PERFORMANCE_DATA},
    {L"HKEY_PERFORMANCE_TEXT", HKEY_PERFORMANCE_TEXT},
    {L"HKEY_PERFORMANCE_NLSTEXT", HKEY_PERFORMANCE_NLSTEXT},
    {L"HKEY_CURRENT_CONFIG", HKEY_CURRENT_CONFIG},
    {L"HKEY_DYN_DATA", HKEY_DYN_DATA}};

// These functions perform case independent path comparisons.
bool EqualPath(const std::wstring& first, const std::wstring& second) {
  return _wcsicmp(first.c_str(), second.c_str()) == 0;
}

bool EqualPath(const std::wstring& first,
               size_t first_offset,
               const std::wstring& second,
               size_t second_offset) {
  return _wcsicmp(first.c_str() + first_offset,
                  second.c_str() + second_offset) == 0;
}

bool EqualPath(const std::wstring& first,
               const wchar_t* second,
               size_t second_len) {
  return _wcsnicmp(first.c_str(), second, second_len) == 0;
}

bool EqualPath(const std::wstring& first,
               size_t first_offset,
               const wchar_t* second,
               size_t second_len) {
  return _wcsnicmp(first.c_str() + first_offset, second, second_len) == 0;
}

// Returns true if |path| starts with "\??\" and returns a path without that
// component.
bool IsNTPath(const std::wstring& path, std::wstring* trimmed_path) {
  if ((path.size() < sandbox::kNTPrefixLen) ||
      !EqualPath(path, sandbox::kNTPrefix, sandbox::kNTPrefixLen)) {
    *trimmed_path = path;
    return false;
  }

  *trimmed_path = path.substr(sandbox::kNTPrefixLen);
  return true;
}

// Returns true if |path| starts with "\Device\" and returns a path without that
// component.
bool IsDevicePath(const std::wstring& path, std::wstring* trimmed_path) {
  if ((path.size() < sandbox::kNTDevicePrefixLen) ||
      (!EqualPath(path, sandbox::kNTDevicePrefix,
                  sandbox::kNTDevicePrefixLen))) {
    *trimmed_path = path;
    return false;
  }

  *trimmed_path = path.substr(sandbox::kNTDevicePrefixLen);
  return true;
}

// Returns the offset to the path seperator following
// "\Device\HarddiskVolumeX" in |path|.
size_t PassHarddiskVolume(const std::wstring& path) {
  static constexpr wchar_t pattern[] = L"\\Device\\HarddiskVolume";
  const size_t patternLen = std::size(pattern) - 1;

  // First, check for |pattern|.
  if ((path.size() < patternLen) || (!EqualPath(path, pattern, patternLen)))
    return std::wstring::npos;

  // Find the next path separator, after the pattern match.
  return path.find_first_of(L'\\', patternLen - 1);
}

// Returns true if |path| starts with "\Device\HarddiskVolumeX\" and returns a
// path without that component.  |removed| will hold the prefix removed.
bool IsDeviceHarddiskPath(const std::wstring& path,
                          std::wstring* trimmed_path,
                          std::wstring* removed) {
  size_t offset = PassHarddiskVolume(path);
  if (offset == std::wstring::npos)
    return false;

  // Remove up to and including the path separator.
  *removed = path.substr(0, offset + 1);
  // Remaining path starts after the path separator.
  *trimmed_path = path.substr(offset + 1);
  return true;
}

bool StartsWithDriveLetter(const std::wstring& path) {
  if (path.size() < kDriveLetterLen)
    return false;

  if (path[1] != L':' || path[2] != L'\\')
    return false;

  return base::IsAsciiAlpha(path[0]);
}

// Removes "\\\\.\\" from the path.
void RemoveImpliedDevice(std::wstring* path) {
  if (EqualPath(*path, kNTDotPrefix, kNTDotPrefixLen))
    *path = path->substr(kNTDotPrefixLen);
}

NTSTATUS WrapQueryObject(HANDLE handle,
                         OBJECT_INFORMATION_CLASS info_class,
                         std::vector<uint8_t>& buffer,
                         PULONG reqd) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    return STATUS_INVALID_PARAMETER;
  NtQueryObjectFunction NtQueryObject = sandbox::GetNtExports()->QueryObject;
  ULONG size = static_cast<ULONG>(buffer.size());
  __try {
    return NtQueryObject(handle, info_class, buffer.data(), size, reqd);
  } __except (GetExceptionCode() == STATUS_INVALID_HANDLE
                  ? EXCEPTION_EXECUTE_HANDLER
                  : EXCEPTION_CONTINUE_SEARCH) {
    return STATUS_INVALID_PARAMETER;
  }
}

// `hint` is used for the initial call to NtQueryObject. Note that some data
// in the returned vector might be unused.
std::unique_ptr<std::vector<uint8_t>> QueryObjectInformation(
    HANDLE handle,
    OBJECT_INFORMATION_CLASS info_class,
    ULONG hint) {
  // Internal pointers in this buffer cannot move about so cannot just return
  // the vector.
  auto data = std::make_unique<std::vector<uint8_t>>(hint);
  ULONG req = 0;
  NTSTATUS ret = WrapQueryObject(handle, info_class, *data, &req);
  if (ret == STATUS_INFO_LENGTH_MISMATCH || ret == STATUS_BUFFER_TOO_SMALL ||
      ret == STATUS_BUFFER_OVERFLOW) {
    data->resize(req);
    ret = WrapQueryObject(handle, info_class, *data, nullptr);
  }
  if (!NT_SUCCESS(ret))
    return nullptr;
  return data;
}

}  // namespace

namespace sandbox {

// Returns true if the provided path points to a pipe.
bool IsPipe(const std::wstring& path) {
  size_t start = 0;
  if (EqualPath(path, sandbox::kNTPrefix, sandbox::kNTPrefixLen))
    start = sandbox::kNTPrefixLen;

  const wchar_t kPipe[] = L"pipe\\";
  if (path.size() < start + std::size(kPipe) - 1)
    return false;

  return EqualPath(path, start, kPipe, std::size(kPipe) - 1);
}

absl::optional<std::wstring> ResolveRegistryName(std::wstring name) {
  for (size_t i = 0; i < std::size(kKnownKey); ++i) {
    if (name.find(kKnownKey[i].name) == 0) {
      HKEY key;
      DWORD disposition;
      if (ERROR_SUCCESS != ::RegCreateKeyEx(kKnownKey[i].key, L"", 0, nullptr,
                                            0, MAXIMUM_ALLOWED, nullptr, &key,
                                            &disposition)) {
        return absl::nullopt;
      }

      auto result = GetPathFromHandle(key);
      ::RegCloseKey(key);

      if (!result)
        return absl::nullopt;

      result->append(name.substr(wcslen(kKnownKey[i].name)));
      return result;
    }
  }
  return absl::nullopt;
}

// |full_path| can have any of the following forms:
//    \??\c:\some\foo\bar
//    \Device\HarddiskVolume0\some\foo\bar
//    \??\HarddiskVolume0\some\foo\bar
DWORD IsReparsePoint(const std::wstring& full_path) {
  // Check if it's a pipe. We can't query the attributes of a pipe.
  if (IsPipe(full_path))
    return ERROR_NOT_A_REPARSE_POINT;

  std::wstring path;
  bool nt_path = IsNTPath(full_path, &path);
  bool has_drive = StartsWithDriveLetter(path);
  bool is_device_path = IsDevicePath(path, &path);

  if (!has_drive && !is_device_path && !nt_path)
    return ERROR_INVALID_NAME;

  bool added_implied_device = false;
  if (!has_drive) {
    path = std::wstring(kNTDotPrefix) + path;
    added_implied_device = true;
  }

  std::wstring::size_type last_pos = std::wstring::npos;
  bool passed_once = false;

  do {
    path = path.substr(0, last_pos);

    DWORD attributes = ::GetFileAttributes(path.c_str());
    if (INVALID_FILE_ATTRIBUTES == attributes) {
      DWORD error = ::GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND &&
          error != ERROR_INVALID_NAME) {
        // Unexpected error.
        if (passed_once && added_implied_device &&
            (path.rfind(L'\\') == kNTDotPrefixLen - 1)) {
          break;
        }
        return error;
      }
    } else if (FILE_ATTRIBUTE_REPARSE_POINT & attributes) {
      // This is a reparse point.
      return ERROR_SUCCESS;
    }

    passed_once = true;
    last_pos = path.rfind(L'\\');
  } while (last_pos > 2);  // Skip root dir.

  return ERROR_NOT_A_REPARSE_POINT;
}

// We get a |full_path| of the forms accepted by IsReparsePoint(), and the name
// we'll get from |handle| will be \device\harddiskvolume1\some\foo\bar.
bool SameObject(HANDLE handle, const wchar_t* full_path) {
  // Check if it's a pipe.
  if (IsPipe(full_path))
    return true;

  auto actual_path = GetPathFromHandle(handle);
  if (!actual_path)
    return false;

  std::wstring path(full_path);
  DCHECK_NT(!path.empty());

  // This may end with a backslash.
  const wchar_t kBackslash = '\\';
  if (path.back() == kBackslash)
    path = path.substr(0, path.length() - 1);

  // Perfect match (case-insensitive check).
  if (EqualPath(actual_path.value(), path))
    return true;

  bool nt_path = IsNTPath(path, &path);
  bool has_drive = StartsWithDriveLetter(path);

  if (!has_drive && nt_path) {
    std::wstring simple_actual_path;
    if (!IsDevicePath(actual_path.value(), &simple_actual_path))
      return false;

    // Perfect match (case-insensitive check).
    return (EqualPath(simple_actual_path, path));
  }

  if (!has_drive)
    return false;

  // We only need 3 chars, but let's alloc a buffer for four.
  wchar_t drive[4] = {0};
  wchar_t vol_name[MAX_PATH];
  memcpy(drive, &path[0], 2 * sizeof(*drive));

  // We'll get a double null terminated string.
  DWORD vol_length = ::QueryDosDeviceW(drive, vol_name, MAX_PATH);
  if (vol_length < 2 || vol_length == MAX_PATH)
    return false;

  // Ignore the nulls at the end.
  vol_length = static_cast<DWORD>(wcslen(vol_name));

  // The two paths should be the same length.
  if (vol_length + path.size() - 2 != actual_path->size())
    return false;

  // Check up to the drive letter.
  if (!EqualPath(actual_path.value(), vol_name, vol_length))
    return false;

  // Check the path after the drive letter.
  if (!EqualPath(actual_path.value(), vol_length, path, 2))
    return false;

  return true;
}

// Just make a best effort here.  There are lots of corner cases that we're
// not expecting - and will fail to make long.
bool ConvertToLongPath(std::wstring* native_path,
                       const std::wstring* drive_letter) {
  if (IsPipe(*native_path))
    return true;

  bool is_device_harddisk_path = false;
  bool is_nt_path = false;
  bool added_implied_device = false;
  std::wstring temp_path;
  std::wstring to_restore;

  // Process a few prefix types.
  if (IsNTPath(*native_path, &temp_path)) {
    // "\??\"
    if (!StartsWithDriveLetter(temp_path)) {
      // Prepend with "\\.\".
      temp_path = std::wstring(kNTDotPrefix) + temp_path;
      added_implied_device = true;
    }
    is_nt_path = true;
  } else if (IsDeviceHarddiskPath(*native_path, &temp_path, &to_restore)) {
    // "\Device\HarddiskVolumeX\" - hacky attempt making ::GetLongPathName
    // work for native device paths.  Remove "\Device\HarddiskVolumeX\" and
    // replace with drive letter.

    // Nothing we can do if we don't have a drive letter.  Leave |native_path|
    // as is.
    if (!drive_letter || drive_letter->empty())
      return false;
    temp_path = *drive_letter + temp_path;
    is_device_harddisk_path = true;
  } else if (IsDevicePath(*native_path, &temp_path)) {
    // "\Device\" - there's nothing we can do to convert to long here.
    return false;
  }

  DWORD size = MAX_PATH;
  std::unique_ptr<wchar_t[]> long_path_buf(new wchar_t[size]);

  DWORD return_value =
      ::GetLongPathName(temp_path.c_str(), long_path_buf.get(), size);
  while (return_value >= size) {
    size *= 2;
    long_path_buf.reset(new wchar_t[size]);
    return_value =
        ::GetLongPathName(temp_path.c_str(), long_path_buf.get(), size);
  }

  DWORD last_error = ::GetLastError();
  if (0 == return_value && (ERROR_FILE_NOT_FOUND == last_error ||
                            ERROR_PATH_NOT_FOUND == last_error ||
                            ERROR_INVALID_NAME == last_error)) {
    // The file does not exist, but maybe a sub path needs to be expanded.
    std::wstring::size_type last_slash = temp_path.rfind(L'\\');
    if (std::wstring::npos == last_slash)
      return false;

    std::wstring begin = temp_path.substr(0, last_slash);
    std::wstring end = temp_path.substr(last_slash);
    if (!ConvertToLongPath(&begin))
      return false;

    // Ok, it worked. Let's reset the return value.
    temp_path = begin + end;
    return_value = 1;
  } else if (0 != return_value) {
    temp_path = long_path_buf.get();
  }

  // If successful, re-apply original namespace prefix before returning.
  if (return_value != 0) {
    if (added_implied_device)
      RemoveImpliedDevice(&temp_path);

    if (is_nt_path) {
      *native_path = kNTPrefix;
      *native_path += temp_path;
    } else if (is_device_harddisk_path) {
      // Remove the added drive letter.
      temp_path = temp_path.substr(kDriveLetterLen);
      *native_path = to_restore;
      *native_path += temp_path;
    } else {
      *native_path = temp_path;
    }

    return true;
  }

  return false;
}

absl::optional<std::wstring> GetNtPathFromWin32Path(const std::wstring& path) {
  base::win::ScopedHandle file(::CreateFileW(
      path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!file.IsValid())
    return absl::nullopt;
  return GetPathFromHandle(file.Get());
}

absl::optional<std::wstring> GetPathFromHandle(HANDLE handle) {
  auto buffer = QueryObjectInformation(handle, ObjectNameInformation, 512);
  if (!buffer)
    return absl::nullopt;
  OBJECT_NAME_INFORMATION* name =
      reinterpret_cast<OBJECT_NAME_INFORMATION*>(buffer->data());
  return std::wstring(
      name->Name.Buffer,
      name->Name.Length / sizeof(name->Name.Buffer[0]));
}

absl::optional<std::wstring> GetTypeNameFromHandle(HANDLE handle) {
  // No typename is currently longer than 32 characters on Windows 11, so use a
  // hint of 128 bytes.
  auto buffer = QueryObjectInformation(handle, ObjectTypeInformation, 128);
  if (!buffer)
    return absl::nullopt;
  OBJECT_TYPE_INFORMATION* name =
      reinterpret_cast<OBJECT_TYPE_INFORMATION*>(buffer->data());
  return std::wstring(name->TypeName.Buffer,
                      name->TypeName.Length / sizeof(name->TypeName.Buffer[0]));
}

bool CopyToChildMemory(HANDLE child,
                       const void* local_buffer,
                       size_t buffer_bytes,
                       void** remote_buffer) {
  DCHECK(remote_buffer);
  if (0 == buffer_bytes) {
    *remote_buffer = nullptr;
    return true;
  }

  // Allocate memory in the target process without specifying the address
  void* remote_data = ::VirtualAllocEx(child, nullptr, buffer_bytes, MEM_COMMIT,
                                       PAGE_READWRITE);
  if (!remote_data)
    return false;

  SIZE_T bytes_written;
  bool success = ::WriteProcessMemory(child, remote_data, local_buffer,
                                      buffer_bytes, &bytes_written);
  if (!success || bytes_written != buffer_bytes) {
    ::VirtualFreeEx(child, remote_data, 0, MEM_RELEASE);
    return false;
  }

  *remote_buffer = remote_data;

  return true;
}

DWORD GetLastErrorFromNtStatus(NTSTATUS status) {
  return GetNtExports()->RtlNtStatusToDosError(status);
}

// This function uses the undocumented PEB ImageBaseAddress field to extract
// the base address of the new process.
void* GetProcessBaseAddress(HANDLE process) {
  PROCESS_BASIC_INFORMATION process_basic_info = {};
  NTSTATUS status = GetNtExports()->QueryInformationProcess(
      process, ProcessBasicInformation, &process_basic_info,
      sizeof(process_basic_info), nullptr);
  if (STATUS_SUCCESS != status)
    return nullptr;

  NT_PEB peb = {};
  SIZE_T bytes_read = 0;
  if (!::ReadProcessMemory(process, process_basic_info.PebBaseAddress, &peb,
                           sizeof(peb), &bytes_read) ||
      (sizeof(peb) != bytes_read)) {
    return nullptr;
  }

  void* base_address = peb.ImageBaseAddress;
  char magic[2] = {};
  if (!::ReadProcessMemory(process, base_address, magic, sizeof(magic),
                           &bytes_read) ||
      (sizeof(magic) != bytes_read)) {
    return nullptr;
  }

  if (magic[0] != 'M' || magic[1] != 'Z')
    return nullptr;

#if defined(_M_ARM64)
  // Windows 10 on ARM64 has multi-threaded DLL loading that does not work with
  // the sandbox. (On x86 this gets disabled by hook detection code that was not
  // ported to ARM64). This overwrites the LoaderThreads value in the process
  // parameters part of the PEB, if it is set to the default of 0 (which
  // actually means it defaults to 4 loading threads). This is an undocumented
  // field so there is a, probably small, risk that it might change or move in
  // the future. In order to slightly guard against that we only update if the
  // value is currently 0.
  auto processParameters = reinterpret_cast<uint8_t*>(peb.ProcessParameters);
  const uint32_t loaderThreadsOffset = 0x40c;
  uint32_t maxLoaderThreads = 0;
  BOOL memoryRead = ::ReadProcessMemory(
      process, processParameters + loaderThreadsOffset, &maxLoaderThreads,
      sizeof(maxLoaderThreads), &bytes_read);
  if (memoryRead && (sizeof(maxLoaderThreads) == bytes_read) &&
      (maxLoaderThreads == 0)) {
    maxLoaderThreads = 1;
    auto address = processParameters + loaderThreadsOffset;
    auto length = sizeof(maxLoaderThreads);

    // First, remove the protection.
    DWORD old_protection;
    if (::VirtualProtectEx(process, address, length, PAGE_READWRITE,
                           &old_protection)) {
      ::WriteProcessMemory(process, address, &maxLoaderThreads, length, NULL);

      // Attempt to restore the original protection.
      ::VirtualProtectEx(process, address, length, old_protection,
                         &old_protection);
    }
  }
#endif

  return base_address;
}

absl::optional<ProcessHandleMap> GetCurrentProcessHandles() {
  DWORD handle_count;
  if (!::GetProcessHandleCount(::GetCurrentProcess(), &handle_count))
    return absl::nullopt;

  // The system call will return only handles up to the buffer size so add a
  // margin of error of an additional 1000 handles.
  std::vector<char> buffer((handle_count + 1000) * sizeof(uint32_t));
  DWORD return_length;
  NTSTATUS status = GetNtExports()->QueryInformationProcess(
      ::GetCurrentProcess(), ProcessHandleTable, buffer.data(),
      static_cast<ULONG>(buffer.size()), &return_length);

  if (!NT_SUCCESS(status)) {
    ::SetLastError(GetLastErrorFromNtStatus(status));
    return absl::nullopt;
  }
  DCHECK(buffer.size() >= return_length);
  DCHECK((buffer.size() % sizeof(uint32_t)) == 0);
  ProcessHandleMap handle_map;
  const uint32_t* handle_values = reinterpret_cast<uint32_t*>(buffer.data());
  size_t count = return_length / sizeof(uint32_t);
  for (size_t index = 0; index < count; ++index) {
    HANDLE handle = base::win::Uint32ToHandle(handle_values[index]);
    auto type_name = GetTypeNameFromHandle(handle);
    if (type_name)
      handle_map[type_name.value()].push_back(handle);
  }
  return handle_map;
}

}  // namespace sandbox

void ResolveNTFunctionPtr(const char* name, void* ptr) {
  static volatile HMODULE ntdll = nullptr;

  if (!ntdll) {
    HMODULE ntdll_local = ::GetModuleHandle(sandbox::kNtdllName);
    // Use PEImage to sanity-check that we have a valid ntdll handle.
    base::win::PEImage ntdll_peimage(ntdll_local);
    CHECK_NT(ntdll_peimage.VerifyMagic());
    // Race-safe way to set static ntdll.
    ::InterlockedCompareExchangePointer(
        reinterpret_cast<PVOID volatile*>(&ntdll), ntdll_local, nullptr);
  }

  CHECK_NT(ntdll);
  FARPROC* function_ptr = reinterpret_cast<FARPROC*>(ptr);
  *function_ptr = ::GetProcAddress(ntdll, name);
  CHECK_NT(*function_ptr);
}
