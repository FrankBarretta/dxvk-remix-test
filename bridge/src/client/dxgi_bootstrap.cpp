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

#include <dxgi.h>
#include <dxgi1_2.h>

#include "client_api_forwarder.h"
#include "bridge_client_bootstrap.h"
#include "dxgi_proxy.h"

namespace {

  HMODULE getSystemDxgiModule() {
    static HMODULE s_module = bridge::client::loadSystemLibrary("dxgi.dll");
    return s_module;
  }

  IDXGIFactory1* createUnderlyingFactory1() {
    using Proc = HRESULT (WINAPI*)(REFIID, void**);

    const HMODULE module = getSystemDxgiModule();
    if (module == nullptr) {
      return nullptr;
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "CreateDXGIFactory1");
    if (proc == nullptr) {
      return nullptr;
    }

    IDXGIFactory1* pFactory = nullptr;
    if (FAILED(proc(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory)))) {
      return nullptr;
    }

    return pFactory;
  }

  IDXGIFactory1* createUnderlyingFactory2(UINT flags) {
    using Proc = HRESULT (WINAPI*)(UINT, REFIID, void**);

    const HMODULE module = getSystemDxgiModule();
    if (module == nullptr) {
      return nullptr;
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "CreateDXGIFactory2");
    if (proc == nullptr) {
      return nullptr;
    }

    IDXGIFactory2* pFactory = nullptr;
    if (FAILED(proc(flags, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&pFactory)))) {
      return nullptr;
    }

    return pFactory;
  }

  void logDxgiShimModeOnce() {
    static bool s_logged = false;

    if (!s_logged) {
      bridge::client::debugLog("RTX Remix Bridge x86 DXGI shim is forwarding to system dxgi.dll. Native DX11 x86 bridge forwarding is not implemented yet.");
      s_logged = true;
    }
  }

}

extern "C" {

  BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
      DisableThreadLibraryCalls(hinst);
      bridge::client::setBootstrapModule(hinst);

      if (getSystemDxgiModule() == nullptr) {
        bridge::client::debugLog("RTX Remix Bridge x86 DXGI shim failed to preload system dxgi.dll.");
      }
    } else if (reason == DLL_PROCESS_DETACH) {
      bridge::client::detachBridge();
    }

    return TRUE;
  }

  HRESULT __stdcall CreateDXGIFactory(REFIID riid, void** ppFactory) {
    using Proc = HRESULT (WINAPI*)(REFIID, void**);

    logDxgiShimModeOnce();
    bridge::client::ensureBridgeAttached("d3d11");

    if (riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2)) {
      IDXGIFactory1* pUnderlyingFactory = createUnderlyingFactory1();
      if (pUnderlyingFactory == nullptr) {
        return bridge::client::missingModuleHr("dxgi.dll");
      }

      return bridge::client::createProxyDxgiFactory(riid, ppFactory, pUnderlyingFactory);
    }

    const HMODULE module = getSystemDxgiModule();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("dxgi.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "CreateDXGIFactory");
    return proc != nullptr ? proc(riid, ppFactory) : E_NOTIMPL;
  }

  HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    logDxgiShimModeOnce();
    bridge::client::ensureBridgeAttached("d3d11");

    IDXGIFactory1* pUnderlyingFactory = riid == __uuidof(IDXGIFactory2)
      ? createUnderlyingFactory2(0)
      : createUnderlyingFactory1();
    if (pUnderlyingFactory == nullptr) {
      return bridge::client::missingModuleHr("dxgi.dll");
    }

    return bridge::client::createProxyDxgiFactory(riid, ppFactory, pUnderlyingFactory);
  }

  HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void** ppFactory) {
    using Proc = HRESULT (WINAPI*)(UINT, REFIID, void**);

    logDxgiShimModeOnce();
    bridge::client::ensureBridgeAttached("d3d11");

    if (riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2)) {
      IDXGIFactory1* pUnderlyingFactory = createUnderlyingFactory2(Flags);
      if (pUnderlyingFactory == nullptr) {
        return bridge::client::missingModuleHr("dxgi.dll");
      }

      return bridge::client::createProxyDxgiFactory(riid, ppFactory, pUnderlyingFactory);
    }

    const HMODULE module = getSystemDxgiModule();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("dxgi.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "CreateDXGIFactory2");
    return proc != nullptr ? proc(Flags, riid, ppFactory) : E_NOTIMPL;
  }

  HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
    using Proc = HRESULT (WINAPI*)();

    logDxgiShimModeOnce();
    bridge::client::ensureBridgeAttached("d3d11");

    const HMODULE module = getSystemDxgiModule();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("dxgi.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "DXGIDeclareAdapterRemovalSupport");
    return proc != nullptr ? proc() : E_NOTIMPL;
  }

  HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid, void** ppDebug) {
    using Proc = HRESULT (WINAPI*)(UINT, REFIID, void**);

    logDxgiShimModeOnce();
    bridge::client::ensureBridgeAttached("d3d11");

    const HMODULE module = getSystemDxgiModule();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("dxgi.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "DXGIGetDebugInterface1");
    return proc != nullptr ? proc(Flags, riid, ppDebug) : E_NOTIMPL;
  }

}