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

#include <d3d11.h>
#include <d3d11on12.h>
#include <dxgi.h>

#include "bridge_client_bootstrap.h"
#include "client_api_forwarder.h"
#include "d3d11_device_proxy.h"
#include "dxgi_proxy.h"
#include "util_modulecommand.h"

namespace {

  HRESULT sendBridgeCreateDeviceCommand(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE driverType,
    UINT flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT featureLevels, UINT sdkVersion) {
    UINT adapterIndex = bridge::client::kDefaultProxyAdapterIndex;
    if (!bridge::client::tryGetProxyAdapterIndex(pAdapter, adapterIndex)) {
      return S_FALSE;
    }

    UID currentUID = 0;
    {
      ModuleClientCommand command(Commands::ID3D11Bridge_CreateDevice);
      currentUID = command.get_uid();
      command.send_many(adapterIndex, static_cast<UINT>(driverType), flags, featureLevels, sdkVersion);

      if (pFeatureLevels != nullptr && featureLevels > 0) {
        command.send_data(static_cast<uint32_t>(sizeof(D3D_FEATURE_LEVEL) * featureLevels), pFeatureLevels);
      } else {
        command.send_data(0u, nullptr);
      }
    }

    if (Result::Success != ModuleBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, currentUID)) {
      bridge::client::debugLog("RTX Remix Bridge x86 D3D11 shim failed waiting for server-side CreateDevice response.");
      return E_FAIL;
    }

    const HRESULT hr = static_cast<HRESULT>(ModuleBridge::get_data());
    ModuleBridge::pop_front();
    return hr;
  }

  void tryBridgeCreateDevice(IDXGIAdapter* pAdapter, D3D_DRIVER_TYPE driverType,
    UINT flags, const D3D_FEATURE_LEVEL* pFeatureLevels, UINT featureLevels, UINT sdkVersion) {
    const HRESULT bridgeHr = sendBridgeCreateDeviceCommand(pAdapter, driverType, flags, pFeatureLevels, featureLevels, sdkVersion);
    if (FAILED(bridgeHr)) {
      bridge::client::debugLog("RTX Remix Bridge x86 D3D11 shim could not create matching server-side D3D11 device.");
    }
  }

  HMODULE getSystemD3D11Module() {
    static HMODULE s_module = bridge::client::loadSystemLibrary("d3d11.dll");
    return s_module;
  }

  bool shouldBootstrapBridge(IDXGIAdapter* pAdapter) {
    IBridgeDxgiAdapterProxy* pProxy = nullptr;
    const bool isProxy = pAdapter != nullptr
      && SUCCEEDED(pAdapter->QueryInterface(__uuidof(IBridgeDxgiAdapterProxy), reinterpret_cast<void**>(&pProxy)));

    if (pProxy != nullptr) {
      pProxy->Release();
    }

    return !isProxy;
  }

  void logD3D11ShimModeOnce() {
    static bool s_logged = false;

    if (!s_logged) {
      bridge::client::debugLog("RTX Remix Bridge x86 D3D11 shim is forwarding to system d3d11.dll. Native DX11 x86 bridge forwarding is not implemented yet.");
      s_logged = true;
    }
  }

}

extern "C" {

  BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
      DisableThreadLibraryCalls(hinst);
      bridge::client::setBootstrapModule(hinst);

      if (getSystemD3D11Module() == nullptr) {
        bridge::client::debugLog("RTX Remix Bridge x86 D3D11 shim failed to preload system d3d11.dll.");
      }
    } else if (reason == DLL_PROCESS_DETACH) {
      bridge::client::detachBridge();
    }

    return TRUE;
  }

  HRESULT __stdcall D3D11CoreCreateDevice(
          IDXGIFactory*       pFactory,
          IDXGIAdapter*       pAdapter,
          UINT                Flags,
    const D3D_FEATURE_LEVEL*  pFeatureLevels,
          UINT                FeatureLevels,
          ID3D11Device**      ppDevice) {
    using Proc = HRESULT (WINAPI*)(IDXGIFactory*, IDXGIAdapter*, UINT, const D3D_FEATURE_LEVEL*, UINT, ID3D11Device**);

    logD3D11ShimModeOnce();
    if (shouldBootstrapBridge(pAdapter)) {
      bridge::client::ensureBridgeAttached("d3d11");
    }
    tryBridgeCreateDevice(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, Flags, pFeatureLevels, FeatureLevels, D3D11_SDK_VERSION);

    const HMODULE module = getSystemD3D11Module();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("d3d11.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "D3D11CoreCreateDevice");
    IDXGIAdapter* pUnwrappedAdapter = bridge::client::unwrapProxyAdapter(pAdapter);
    const HRESULT hr = proc != nullptr
      ? proc(pFactory, pUnwrappedAdapter, Flags, pFeatureLevels, FeatureLevels, ppDevice)
      : E_NOTIMPL;

    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
      bridge::client::createProxyD3D11Device(ppDevice);
    }

    if (pUnwrappedAdapter != nullptr && pUnwrappedAdapter != pAdapter) {
      pUnwrappedAdapter->Release();
    }

    return hr;
  }

  HRESULT __stdcall D3D11CreateDevice(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    using Proc = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    logD3D11ShimModeOnce();
    if (shouldBootstrapBridge(pAdapter)) {
      bridge::client::ensureBridgeAttached("d3d11");
    }
    tryBridgeCreateDevice(pAdapter, DriverType, Flags, pFeatureLevels, FeatureLevels, SDKVersion);

    const HMODULE module = getSystemD3D11Module();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("d3d11.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "D3D11CreateDevice");
    IDXGIAdapter* pUnwrappedAdapter = bridge::client::unwrapProxyAdapter(pAdapter);
    const HRESULT hr = proc != nullptr
      ? proc(pUnwrappedAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, ppDevice, pFeatureLevel, ppImmediateContext)
      : E_NOTIMPL;

    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
      bridge::client::createProxyD3D11Device(ppDevice);
    }

    if (pUnwrappedAdapter != nullptr && pUnwrappedAdapter != pAdapter) {
      pUnwrappedAdapter->Release();
    }

    return hr;
  }

  HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
          IDXGIAdapter*         pAdapter,
          D3D_DRIVER_TYPE       DriverType,
          HMODULE               Software,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          UINT                  SDKVersion,
    const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
          IDXGISwapChain**      ppSwapChain,
          ID3D11Device**        ppDevice,
          D3D_FEATURE_LEVEL*    pFeatureLevel,
          ID3D11DeviceContext** ppImmediateContext) {
    using Proc = HRESULT (WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

    logD3D11ShimModeOnce();
    if (shouldBootstrapBridge(pAdapter)) {
      bridge::client::ensureBridgeAttached("d3d11");
    }
    tryBridgeCreateDevice(pAdapter, DriverType, Flags, pFeatureLevels, FeatureLevels, SDKVersion);

    const HMODULE module = getSystemD3D11Module();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("d3d11.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "D3D11CreateDeviceAndSwapChain");
    IDXGIAdapter* pUnwrappedAdapter = bridge::client::unwrapProxyAdapter(pAdapter);
    const HRESULT hr = proc != nullptr
      ? proc(pUnwrappedAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels, SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel, ppImmediateContext)
      : E_NOTIMPL;

    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
      bridge::client::createProxyD3D11Device(ppDevice);
    }

    if (pUnwrappedAdapter != nullptr && pUnwrappedAdapter != pAdapter) {
      pUnwrappedAdapter->Release();
    }

    return hr;
  }

  HRESULT __stdcall D3D11On12CreateDevice(
          IUnknown*             pDevice,
          UINT                  Flags,
    const D3D_FEATURE_LEVEL*    pFeatureLevels,
          UINT                  FeatureLevels,
          IUnknown* const*      ppCommandQueues,
          UINT                  NumQueues,
          UINT                  NodeMask,
          ID3D11Device**        ppDevice,
          ID3D11DeviceContext** ppImmediateContext,
          D3D_FEATURE_LEVEL*    pChosenFeatureLevel) {
    using Proc = HRESULT (WINAPI*)(IUnknown*, UINT, const D3D_FEATURE_LEVEL*, UINT, IUnknown* const*, UINT, UINT, ID3D11Device**, ID3D11DeviceContext**, D3D_FEATURE_LEVEL*);

    logD3D11ShimModeOnce();
    bridge::client::ensureBridgeAttached("d3d11");

    const HMODULE module = getSystemD3D11Module();
    if (module == nullptr) {
      return bridge::client::missingModuleHr("d3d11.dll");
    }

    const auto proc = bridge::client::loadProc<Proc>(module, "D3D11On12CreateDevice");
    const HRESULT hr = proc != nullptr
      ? proc(pDevice, Flags, pFeatureLevels, FeatureLevels, ppCommandQueues, NumQueues, NodeMask, ppDevice, ppImmediateContext, pChosenFeatureLevel)
      : E_NOTIMPL;

    if (SUCCEEDED(hr) && ppDevice != nullptr && *ppDevice != nullptr) {
      bridge::client::createProxyD3D11Device(ppDevice);
    }

    return hr;
  }

}