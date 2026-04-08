/*
 * Copyright (c) 2022-2024, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include <strsafe.h>
#include <windows.h>

namespace bridge::client {

  inline void debugLog(const char* message) {
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
  }

  inline HMODULE loadSystemLibrary(const char* libraryName) {
    char systemPath[MAX_PATH] = {};

    if (GetSystemDirectoryA(systemPath, ARRAYSIZE(systemPath)) == 0) {
      return nullptr;
    }

    if (FAILED(StringCchCatA(systemPath, ARRAYSIZE(systemPath), "\\"))) {
      return nullptr;
    }

    if (FAILED(StringCchCatA(systemPath, ARRAYSIZE(systemPath), libraryName))) {
      return nullptr;
    }

    return LoadLibraryA(systemPath);
  }

  template <typename Proc>
  inline Proc loadProc(HMODULE module, const char* procName) {
    return module != nullptr
      ? reinterpret_cast<Proc>(GetProcAddress(module, procName))
      : nullptr;
  }

  inline HRESULT missingModuleHr(const char* moduleName) {
    char message[256] = {};
    StringCchPrintfA(message, ARRAYSIZE(message), "RTX Remix Bridge x86 shim failed to load system %s.", moduleName);
    debugLog(message);
    return HRESULT_FROM_WIN32(GetLastError());
  }

}