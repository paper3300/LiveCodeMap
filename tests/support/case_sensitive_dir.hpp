// Test support: per-directory case sensitivity on Windows (Win32
// FileCaseSensitiveInfo), applied only to an isolated test directory. Never
// changes any system-wide setting. Returns false when unsupported so tests can
// report the limitation instead of faking the case.
#pragma once

#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace lcm::test_support {

inline bool try_enable_case_sensitive_directory(const std::filesystem::path& dir) {
#ifdef _WIN32
  HANDLE handle = CreateFileW(dir.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  FILE_CASE_SENSITIVE_INFO info{};
  info.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
  const BOOL ok = SetFileInformationByHandle(handle, FileCaseSensitiveInfo, &info, sizeof(info));
  CloseHandle(handle);
  return ok != 0;
#else
  (void)dir;
  return false;
#endif
}

}  // namespace lcm::test_support
