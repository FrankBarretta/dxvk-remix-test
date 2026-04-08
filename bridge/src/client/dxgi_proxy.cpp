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

#include "dxgi_proxy.h"

#include "d3d11_device_proxy.h"
#include "config/global_options.h"
#include "log/log.h"

#include "util_modulecommand.h"

#include <utility>

namespace bridge::client {

  namespace {

    class DxgiOutputProxy final : public IDXGIOutput, public IBridgeDxgiOutputProxy {
    public:
      DxgiOutputProxy(IDXGIOutput* pOutput, IDXGIAdapter1* pAdapter, IUnknown* pFactory)
      : m_pOutput(pOutput), m_pAdapter(pAdapter), m_pFactory(pFactory) {
        if (m_pAdapter != nullptr) {
          m_pAdapter->AddRef();
        }

        if (m_pFactory != nullptr) {
          m_pFactory->AddRef();
        }
      }

      ~DxgiOutputProxy();

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_refCount;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refCount = --m_refCount;
        if (refCount == 0) {
          delete this;
        }
        return refCount;
      }

      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_pOutput->SetPrivateData(Name, DataSize, pData);
      }

      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_pOutput->SetPrivateDataInterface(Name, pUnknown);
      }

      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_pOutput->GetPrivateData(Name, pDataSize, pData);
      }

      HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;
      HRESULT STDMETHODCALLTYPE GetDesc(DXGI_OUTPUT_DESC* pDesc) override {
        return m_pOutput->GetDesc(pDesc);
      }

      HRESULT STDMETHODCALLTYPE GetDisplayModeList(DXGI_FORMAT EnumFormat, UINT Flags, UINT* pNumModes, DXGI_MODE_DESC* pDesc) override {
        return m_pOutput->GetDisplayModeList(EnumFormat, Flags, pNumModes, pDesc);
      }

      HRESULT STDMETHODCALLTYPE FindClosestMatchingMode(const DXGI_MODE_DESC* pModeToMatch, DXGI_MODE_DESC* pClosestMatch, IUnknown* pConcernedDevice) override {
        return m_pOutput->FindClosestMatchingMode(pModeToMatch, pClosestMatch, pConcernedDevice);
      }

      HRESULT STDMETHODCALLTYPE WaitForVBlank() override {
        return m_pOutput->WaitForVBlank();
      }

      HRESULT STDMETHODCALLTYPE TakeOwnership(IUnknown* pDevice, BOOL Exclusive) override {
        return m_pOutput->TakeOwnership(pDevice, Exclusive);
      }

      void STDMETHODCALLTYPE ReleaseOwnership() override {
        m_pOutput->ReleaseOwnership();
      }

      HRESULT STDMETHODCALLTYPE GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES* pGammaCaps) override {
        return m_pOutput->GetGammaControlCapabilities(pGammaCaps);
      }

      HRESULT STDMETHODCALLTYPE SetGammaControl(const DXGI_GAMMA_CONTROL* pArray) override {
        return m_pOutput->SetGammaControl(pArray);
      }

      HRESULT STDMETHODCALLTYPE GetGammaControl(DXGI_GAMMA_CONTROL* pArray) override {
        return m_pOutput->GetGammaControl(pArray);
      }

      HRESULT STDMETHODCALLTYPE SetDisplaySurface(IDXGISurface* pScanoutSurface) override {
        return m_pOutput->SetDisplaySurface(pScanoutSurface);
      }

      HRESULT STDMETHODCALLTYPE GetDisplaySurfaceData(IDXGISurface* pDestination) override {
        return m_pOutput->GetDisplaySurfaceData(pDestination);
      }

      HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
        return m_pOutput->GetFrameStatistics(pStats);
      }

      IDXGIOutput* STDMETHODCALLTYPE GetUnderlyingOutput() override {
        if (m_pOutput != nullptr) {
          m_pOutput->AddRef();
        }
        return m_pOutput;
      }

    private:
      std::atomic<ULONG> m_refCount = 1;
      IDXGIOutput* m_pOutput = nullptr;
      IDXGIAdapter1* m_pAdapter = nullptr;
      IUnknown* m_pFactory = nullptr;
    };

    HRESULT sendCreateFactoryCommand() {
      UID currentUID = 0;
      {
        ModuleClientCommand command(Commands::IDXGIFactory1_CreateFactory);
        currentUID = command.get_uid();
      }

      if (Result::Success != ModuleBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, currentUID)) {
        Logger::err("CreateDXGIFactory bridge bootstrap failed with: no response from server.");
        return E_FAIL;
      }

      const HRESULT hr = static_cast<HRESULT>(ModuleBridge::get_data());
      ModuleBridge::pop_front();
      return hr;
    }

    HRESULT sendEnumAdaptersCommand(UINT adapterIndex) {
      UID currentUID = 0;
      {
        ModuleClientCommand command(Commands::IDXGIFactory1_EnumAdapters1);
        currentUID = command.get_uid();
        command.send_data(adapterIndex);
      }

      if (Result::Success != ModuleBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, currentUID)) {
        Logger::err("IDXGIFactory::EnumAdapters bridge call failed with: no response from server.");
        return E_FAIL;
      }

      const HRESULT hr = static_cast<HRESULT>(ModuleBridge::get_data());
      ModuleBridge::pop_front();
      return hr;
    }

    class DxgiAdapterProxy final : public IDXGIAdapter1, public IBridgeDxgiAdapterProxy {
    public:
      DxgiAdapterProxy(IDXGIAdapter1* pAdapter, UINT adapterIndex, IUnknown* pFactory)
      : m_pAdapter(pAdapter), m_adapterIndex(adapterIndex), m_pFactory(pFactory) {
        if (m_pFactory != nullptr) {
          m_pFactory->AddRef();
        }
      }

      ~DxgiAdapterProxy() {
        if (m_pFactory != nullptr) {
          m_pFactory->Release();
        }

        if (m_pAdapter != nullptr) {
          m_pAdapter->Release();
        }
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
          return E_POINTER;
        }

        *ppvObject = nullptr;

        if (riid == __uuidof(IUnknown)
         || riid == __uuidof(IDXGIObject)
         || riid == __uuidof(IDXGIAdapter)
         || riid == __uuidof(IDXGIAdapter1)) {
          *ppvObject = static_cast<IDXGIAdapter1*>(this);
        } else if (riid == __uuidof(IBridgeDxgiAdapterProxy)) {
          *ppvObject = static_cast<IBridgeDxgiAdapterProxy*>(this);
        } else {
          return m_pAdapter->QueryInterface(riid, ppvObject);
        }

        AddRef();
        return S_OK;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_refCount;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refCount = --m_refCount;
        if (refCount == 0) {
          delete this;
        }
        return refCount;
      }

      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_pAdapter->SetPrivateData(Name, DataSize, pData);
      }

      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_pAdapter->SetPrivateDataInterface(Name, pUnknown);
      }

      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_pAdapter->GetPrivateData(Name, pDataSize, pData);
      }

      HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        if (ppParent == nullptr) {
          return E_POINTER;
        }

        *ppParent = nullptr;

        if (m_pFactory != nullptr && (riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2))) {
          return m_pFactory->QueryInterface(riid, ppParent);
        }

        return m_pAdapter->GetParent(riid, ppParent);
      }

      HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output, IDXGIOutput** ppOutput) override {
        if (ppOutput == nullptr) {
          return E_INVALIDARG;
        }

        *ppOutput = nullptr;

        IDXGIOutput* pOutput = nullptr;
        const HRESULT hr = m_pAdapter->EnumOutputs(Output, &pOutput);
        if (FAILED(hr)) {
          return hr;
        }

        *ppOutput = new DxgiOutputProxy(pOutput, m_pAdapter, m_pFactory);
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC* pDesc) override {
        return m_pAdapter->GetDesc(pDesc);
      }

      HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) override {
        return m_pAdapter->CheckInterfaceSupport(InterfaceName, pUMDVersion);
      }

      HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* pDesc) override {
        return m_pAdapter->GetDesc1(pDesc);
      }

      IDXGIAdapter1* STDMETHODCALLTYPE GetUnderlyingAdapter() override {
        if (m_pAdapter != nullptr) {
          m_pAdapter->AddRef();
        }
        return m_pAdapter;
      }

      UINT STDMETHODCALLTYPE GetAdapterIndex() override {
        return m_adapterIndex;
      }

    private:
      std::atomic<ULONG> m_refCount = 1;
      IDXGIAdapter1* m_pAdapter = nullptr;
      UINT m_adapterIndex = 0;
      IUnknown* m_pFactory = nullptr;
    };

    class DxgiFactoryProxy final : public IDXGIFactory2 {
    public:
      explicit DxgiFactoryProxy(IDXGIFactory1* pFactory)
      : m_pFactory(pFactory) {
        if (m_pFactory != nullptr) {
          m_pFactory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&m_pFactory2));
        }
      }

      ~DxgiFactoryProxy() {
        if (m_pFactory2 != nullptr) {
          m_pFactory2->Release();
        }

        if (m_pFactory != nullptr) {
          m_pFactory->Release();
        }
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
          return E_POINTER;
        }

        *ppvObject = nullptr;

        if (riid == __uuidof(IUnknown)
         || riid == __uuidof(IDXGIObject)
         || riid == __uuidof(IDXGIFactory)
         || riid == __uuidof(IDXGIFactory1)
         || (riid == __uuidof(IDXGIFactory2) && m_pFactory2 != nullptr)) {
          *ppvObject = static_cast<IDXGIFactory2*>(this);
          AddRef();
          return S_OK;
        }

        return m_pFactory->QueryInterface(riid, ppvObject);
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_refCount;
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refCount = --m_refCount;
        if (refCount == 0) {
          delete this;
        }
        return refCount;
      }

      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_pFactory->SetPrivateData(Name, DataSize, pData);
      }

      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_pFactory->SetPrivateDataInterface(Name, pUnknown);
      }

      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_pFactory->GetPrivateData(Name, pDataSize, pData);
      }

      HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return m_pFactory->GetParent(riid, ppParent);
      }

      HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) override {
        if (ppAdapter == nullptr) {
          return E_INVALIDARG;
        }

        *ppAdapter = nullptr;

        IDXGIAdapter1* pAdapter1 = nullptr;
        const HRESULT localHr = m_pFactory->EnumAdapters1(Adapter, &pAdapter1);
        if (FAILED(localHr)) {
          return localHr;
        }

        const HRESULT bridgeHr = sendEnumAdaptersCommand(Adapter);
        if (FAILED(bridgeHr)) {
          pAdapter1->Release();
          return bridgeHr;
        }

        *ppAdapter = static_cast<IDXGIAdapter*>(new DxgiAdapterProxy(pAdapter1, Adapter, static_cast<IUnknown*>(this)));
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle, UINT Flags) override {
        return m_pFactory->MakeWindowAssociation(WindowHandle, Flags);
      }

      HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* pWindowHandle) override {
        return m_pFactory->GetWindowAssociation(pWindowHandle);
      }

      HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) override {
        ID3D11Device* pD3D11Device = nullptr;
        IUnknown* pEffectiveDevice = pDevice;
        if (pDevice != nullptr && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pD3D11Device)))) {
          pEffectiveDevice = bridge::client::unwrapProxyD3D11Device(pD3D11Device);
        }

        const HRESULT hr = m_pFactory->CreateSwapChain(pEffectiveDevice, pDesc, ppSwapChain);

        if (pEffectiveDevice != nullptr && pEffectiveDevice != pDevice && pEffectiveDevice != pD3D11Device) {
          pEffectiveDevice->Release();
        }

        if (pD3D11Device != nullptr) {
          pD3D11Device->Release();
        }

        return hr;
      }

      HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) override {
        return m_pFactory->CreateSoftwareAdapter(Module, ppAdapter);
      }

      HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) override {
        if (ppAdapter == nullptr) {
          return E_INVALIDARG;
        }

        *ppAdapter = nullptr;

        IDXGIAdapter1* pAdapter1 = nullptr;
        const HRESULT localHr = m_pFactory->EnumAdapters1(Adapter, &pAdapter1);
        if (FAILED(localHr)) {
          return localHr;
        }

        const HRESULT bridgeHr = sendEnumAdaptersCommand(Adapter);
        if (FAILED(bridgeHr)) {
          pAdapter1->Release();
          return bridgeHr;
        }

        *ppAdapter = new DxgiAdapterProxy(pAdapter1, Adapter, static_cast<IUnknown*>(this));
        return S_OK;
      }

      BOOL STDMETHODCALLTYPE IsCurrent() override {
        return m_pFactory->IsCurrent();
      }

      BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override {
        return m_pFactory2 != nullptr
          ? m_pFactory2->IsWindowedStereoEnabled()
          : FALSE;
      }

      HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd,
        const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) override {
        ID3D11Device* pD3D11Device = nullptr;
        IUnknown* pEffectiveDevice = pDevice;
        if (pDevice != nullptr && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pD3D11Device)))) {
          pEffectiveDevice = bridge::client::unwrapProxyD3D11Device(pD3D11Device);
        }
        IDXGIOutput* pUnwrappedOutput = bridge::client::unwrapProxyOutput(pRestrictToOutput);
        const HRESULT hr = m_pFactory2 != nullptr
          ? m_pFactory2->CreateSwapChainForHwnd(pEffectiveDevice, hWnd, pDesc, pFullscreenDesc, pUnwrappedOutput, ppSwapChain)
          : DXGI_ERROR_UNSUPPORTED;

        if (pEffectiveDevice != nullptr && pEffectiveDevice != pDevice && pEffectiveDevice != pD3D11Device) {
          pEffectiveDevice->Release();
        }

        if (pD3D11Device != nullptr) {
          pD3D11Device->Release();
        }

        return hr;
      }

      HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
        const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
        IDXGISwapChain1** ppSwapChain) override {
        ID3D11Device* pD3D11Device = nullptr;
        IUnknown* pEffectiveDevice = pDevice;
        if (pDevice != nullptr && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pD3D11Device)))) {
          pEffectiveDevice = bridge::client::unwrapProxyD3D11Device(pD3D11Device);
        }
        IDXGIOutput* pUnwrappedOutput = bridge::client::unwrapProxyOutput(pRestrictToOutput);
        const HRESULT hr = m_pFactory2 != nullptr
          ? m_pFactory2->CreateSwapChainForCoreWindow(pEffectiveDevice, pWindow, pDesc, pUnwrappedOutput, ppSwapChain)
          : DXGI_ERROR_UNSUPPORTED;

        if (pEffectiveDevice != nullptr && pEffectiveDevice != pDevice && pEffectiveDevice != pD3D11Device) {
          pEffectiveDevice->Release();
        }

        if (pD3D11Device != nullptr) {
          pD3D11Device->Release();
        }

        return hr;
      }

      HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) override {
        return m_pFactory2 != nullptr
          ? m_pFactory2->GetSharedResourceAdapterLuid(hResource, pLuid)
          : DXGI_ERROR_UNSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override {
        return m_pFactory2 != nullptr
          ? m_pFactory2->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie)
          : DXGI_ERROR_UNSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
        return m_pFactory2 != nullptr
          ? m_pFactory2->RegisterStereoStatusEvent(hEvent, pdwCookie)
          : DXGI_ERROR_UNSUPPORTED;
      }

      void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) override {
        if (m_pFactory2 != nullptr) {
          m_pFactory2->UnregisterStereoStatus(dwCookie);
        }
      }

      HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg, DWORD* pdwCookie) override {
        return m_pFactory2 != nullptr
          ? m_pFactory2->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie)
          : DXGI_ERROR_UNSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) override {
        return m_pFactory2 != nullptr
          ? m_pFactory2->RegisterOcclusionStatusEvent(hEvent, pdwCookie)
          : DXGI_ERROR_UNSUPPORTED;
      }

      void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) override {
        if (m_pFactory2 != nullptr) {
          m_pFactory2->UnregisterOcclusionStatus(dwCookie);
        }
      }

      HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* pDevice,
        const DXGI_SWAP_CHAIN_DESC1* pDesc, IDXGIOutput* pRestrictToOutput,
        IDXGISwapChain1** ppSwapChain) override {
        ID3D11Device* pD3D11Device = nullptr;
        IUnknown* pEffectiveDevice = pDevice;
        if (pDevice != nullptr && SUCCEEDED(pDevice->QueryInterface(__uuidof(ID3D11Device), reinterpret_cast<void**>(&pD3D11Device)))) {
          pEffectiveDevice = bridge::client::unwrapProxyD3D11Device(pD3D11Device);
        }
        IDXGIOutput* pUnwrappedOutput = bridge::client::unwrapProxyOutput(pRestrictToOutput);
        const HRESULT hr = m_pFactory2 != nullptr
          ? m_pFactory2->CreateSwapChainForComposition(pEffectiveDevice, pDesc, pUnwrappedOutput, ppSwapChain)
          : DXGI_ERROR_UNSUPPORTED;

        if (pEffectiveDevice != nullptr && pEffectiveDevice != pDevice && pEffectiveDevice != pD3D11Device) {
          pEffectiveDevice->Release();
        }

        if (pD3D11Device != nullptr) {
          pD3D11Device->Release();
        }

        return hr;
      }

    private:
      std::atomic<ULONG> m_refCount = 1;
      IDXGIFactory1* m_pFactory = nullptr;
      IDXGIFactory2* m_pFactory2 = nullptr;
    };

    DxgiOutputProxy::~DxgiOutputProxy() {
      if (m_pFactory != nullptr) {
        m_pFactory->Release();
      }

      if (m_pAdapter != nullptr) {
        m_pAdapter->Release();
      }

      if (m_pOutput != nullptr) {
        m_pOutput->Release();
      }
    }

    HRESULT DxgiOutputProxy::QueryInterface(REFIID riid, void** ppvObject) {
      if (ppvObject == nullptr) {
        return E_POINTER;
      }

      *ppvObject = nullptr;

      if (riid == __uuidof(IUnknown)
       || riid == __uuidof(IDXGIObject)
       || riid == __uuidof(IDXGIOutput)) {
        *ppvObject = static_cast<IDXGIOutput*>(this);
      } else if (riid == __uuidof(IBridgeDxgiOutputProxy)) {
        *ppvObject = static_cast<IBridgeDxgiOutputProxy*>(this);
      } else {
        return m_pOutput->QueryInterface(riid, ppvObject);
      }

      AddRef();
      return S_OK;
    }

    HRESULT DxgiOutputProxy::GetParent(REFIID riid, void** ppParent) {
      if (ppParent == nullptr) {
        return E_POINTER;
      }

      *ppParent = nullptr;

      if (m_pAdapter != nullptr && (riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1))) {
        return m_pAdapter->QueryInterface(riid, ppParent);
      }

      if (m_pFactory != nullptr && (riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2))) {
        return m_pFactory->QueryInterface(riid, ppParent);
      }

      return m_pOutput->GetParent(riid, ppParent);
    }

  }

  HRESULT createProxyDxgiFactory(REFIID riid, void** ppFactory, IDXGIFactory1* pUnderlyingFactory) {
    if (ppFactory == nullptr) {
      return E_POINTER;
    }

    *ppFactory = nullptr;

    if (pUnderlyingFactory == nullptr) {
      return E_INVALIDARG;
    }

    const HRESULT bridgeHr = sendCreateFactoryCommand();
    if (FAILED(bridgeHr)) {
      return bridgeHr;
    }

    DxgiFactoryProxy* pProxy = new DxgiFactoryProxy(pUnderlyingFactory);
    const HRESULT hr = pProxy->QueryInterface(riid, ppFactory);
    pProxy->Release();
    return hr;
  }

}