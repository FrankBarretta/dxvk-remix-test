#pragma once

#include <cstdio>

#include <windows.h>

namespace dxvk {

  inline void DxvkEarlyTrace(const char* message) {
    char line[1024];
    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();
    const int lineLength = std::snprintf(line, sizeof(line), "[pid=%lu tid=%lu] %s\r\n", pid, tid, message);

    if (lineLength > 0)
      OutputDebugStringA(line);

    HMODULE module = nullptr;

    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&DxvkEarlyTrace),
        &module)) {
      module = nullptr;
    }

    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = module != nullptr
      ? GetModuleFileNameW(module, path, MAX_PATH)
      : GetModuleFileNameW(nullptr, path, MAX_PATH);

    if (pathLength == 0 || pathLength >= MAX_PATH)
      return;

    wchar_t* lastSeparator = wcsrchr(path, L'\\');

    if (lastSeparator == nullptr)
      return;

    *(lastSeparator + 1) = L'\0';
    wcscat_s(path, L"dxvk-device-early.log");

    HANDLE file = CreateFileW(path,
      FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);

    if (file == INVALID_HANDLE_VALUE)
      return;

    DWORD bytesWritten = 0;
    WriteFile(file, line, static_cast<DWORD>(lineLength), &bytesWritten, nullptr);
    CloseHandle(file);
  }

}