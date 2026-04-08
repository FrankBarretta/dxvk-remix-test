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

#include "d3d11_device_proxy.h"

#include <atomic>

#include "bridge_client_bootstrap.h"
#include "util_modulecommand.h"

#include "log/log.h"

namespace bridge::client {

  namespace {

    bool waitForModuleResponse(UID currentUID, HRESULT& hr) {
      if (Result::Success != ModuleBridge::waitForCommand(Commands::Bridge_Response, GlobalOptions::getAckTimeout(), nullptr, true, currentUID)) {
        return false;
      }

      hr = static_cast<HRESULT>(ModuleBridge::get_data());
      ModuleBridge::pop_front();
      return true;
    }

    UINT getBridgeHandle(IUnknown* pObject) {
      return static_cast<UINT>(reinterpret_cast<uintptr_t>(pObject));
    }

    void logTexture2DInitialDataWarningOnce() {
      static bool s_logged = false;

      if (!s_logged) {
        Logger::warn("RTX Remix Bridge x86 D3D11 shadow CreateTexture2D currently skips textures created with initial data.");
        s_logged = true;
      }
    }

    void logUnsupportedRtvResourceWarningOnce() {
      static bool s_logged = false;

      if (!s_logged) {
        Logger::warn("RTX Remix Bridge x86 D3D11 shadow CreateRenderTargetView currently supports only Texture2D-backed resources.");
        s_logged = true;
      }
    }

    void logUnsupportedSrvResourceWarningOnce() {
      static bool s_logged = false;

      if (!s_logged) {
        Logger::warn("RTX Remix Bridge x86 D3D11 shadow CreateShaderResourceView currently supports only Texture2D-backed resources.");
        s_logged = true;
      }
    }

    void logUnsupportedDsvResourceWarningOnce() {
      static bool s_logged = false;

      if (!s_logged) {
        Logger::warn("RTX Remix Bridge x86 D3D11 shadow CreateDepthStencilView currently supports only Texture2D-backed resources.");
        s_logged = true;
      }
    }

    HRESULT tryShadowCreateTexture2D(ID3D11Texture2D* pTexture, const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData) {
      if (pTexture == nullptr || pDesc == nullptr) {
        return E_INVALIDARG;
      }

      if (pInitialData != nullptr) {
        logTexture2DInitialDataWarningOnce();
        return S_FALSE;
      }

      UID currentUID = 0;
      {
        ModuleClientCommand command(Commands::ID3D11Bridge_CreateTexture2D);
        currentUID = command.get_uid();
        command.send_data(getBridgeHandle(pTexture));
        command.send_data(sizeof(D3D11_TEXTURE2D_DESC), pDesc);
      }

      HRESULT hr = E_FAIL;
      return waitForModuleResponse(currentUID, hr) ? hr : E_FAIL;
    }

    HRESULT tryShadowCreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView* pRenderTargetView) {
      if (pResource == nullptr || pRenderTargetView == nullptr) {
        return E_INVALIDARG;
      }

      ID3D11Texture2D* pTexture2D = nullptr;
      const HRESULT textureHr = pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pTexture2D));
      if (FAILED(textureHr) || pTexture2D == nullptr) {
        logUnsupportedRtvResourceWarningOnce();
        return S_FALSE;
      }

      const UINT resourceHandle = getBridgeHandle(pTexture2D);
      pTexture2D->Release();

      UID currentUID = 0;
      {
        ModuleClientCommand command(Commands::ID3D11Bridge_CreateRenderTargetView);
        currentUID = command.get_uid();
        command.send_many(resourceHandle, getBridgeHandle(pRenderTargetView));
        if (pDesc != nullptr) {
          command.send_data(sizeof(D3D11_RENDER_TARGET_VIEW_DESC), pDesc);
        } else {
          command.send_data(0u, nullptr);
        }
      }

      HRESULT hr = E_FAIL;
      return waitForModuleResponse(currentUID, hr) ? hr : E_FAIL;
    }

    HRESULT tryShadowCreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView* pShaderResourceView) {
      if (pResource == nullptr || pShaderResourceView == nullptr) {
        return E_INVALIDARG;
      }

      ID3D11Texture2D* pTexture2D = nullptr;
      const HRESULT textureHr = pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pTexture2D));
      if (FAILED(textureHr) || pTexture2D == nullptr) {
        logUnsupportedSrvResourceWarningOnce();
        return S_FALSE;
      }

      const UINT resourceHandle = getBridgeHandle(pTexture2D);
      pTexture2D->Release();

      UID currentUID = 0;
      {
        ModuleClientCommand command(Commands::ID3D11Bridge_CreateShaderResourceView);
        currentUID = command.get_uid();
        command.send_many(resourceHandle, getBridgeHandle(pShaderResourceView));
        if (pDesc != nullptr) {
          command.send_data(sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC), pDesc);
        } else {
          command.send_data(0u, nullptr);
        }
      }

      HRESULT hr = E_FAIL;
      return waitForModuleResponse(currentUID, hr) ? hr : E_FAIL;
    }

    HRESULT tryShadowCreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView* pDepthStencilView) {
      if (pResource == nullptr || pDepthStencilView == nullptr) {
        return E_INVALIDARG;
      }

      ID3D11Texture2D* pTexture2D = nullptr;
      const HRESULT textureHr = pResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&pTexture2D));
      if (FAILED(textureHr) || pTexture2D == nullptr) {
        logUnsupportedDsvResourceWarningOnce();
        return S_FALSE;
      }

      const UINT resourceHandle = getBridgeHandle(pTexture2D);
      pTexture2D->Release();

      UID currentUID = 0;
      {
        ModuleClientCommand command(Commands::ID3D11Bridge_CreateDepthStencilView);
        currentUID = command.get_uid();
        command.send_many(resourceHandle, getBridgeHandle(pDepthStencilView));
        if (pDesc != nullptr) {
          command.send_data(sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC), pDesc);
        } else {
          command.send_data(0u, nullptr);
        }
      }

      HRESULT hr = E_FAIL;
      return waitForModuleResponse(currentUID, hr) ? hr : E_FAIL;
    }

    template <typename TDesc>
    HRESULT tryShadowCreateStateObject(Commands::D3D9Command commandId, IUnknown* pStateObject, const TDesc* pDesc) {
      if (pStateObject == nullptr || pDesc == nullptr) {
        return E_INVALIDARG;
      }

      UID currentUID = 0;
      {
        ModuleClientCommand command(commandId);
        currentUID = command.get_uid();
        command.send_data(getBridgeHandle(pStateObject));
        command.send_data(sizeof(TDesc), pDesc);
      }

      HRESULT hr = E_FAIL;
      return waitForModuleResponse(currentUID, hr) ? hr : E_FAIL;
    }

    class D3D11DeviceProxy final : public ID3D11Device, public IBridgeD3D11DeviceProxy {
    public:
      explicit D3D11DeviceProxy(ID3D11Device* pDevice)
      : m_pDevice(pDevice) {
      }

      ~D3D11DeviceProxy() {
        if (m_pDevice != nullptr) {
          m_pDevice->Release();
        }
      }

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (ppvObject == nullptr) {
          return E_POINTER;
        }

        *ppvObject = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device)) {
          *ppvObject = static_cast<ID3D11Device*>(this);
        } else if (riid == __uuidof(IBridgeD3D11DeviceProxy)) {
          *ppvObject = static_cast<IBridgeD3D11DeviceProxy*>(this);
        } else {
          return m_pDevice->QueryInterface(riid, ppvObject);
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

      HRESULT STDMETHODCALLTYPE CreateBuffer(const D3D11_BUFFER_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Buffer** ppBuffer) override { return m_pDevice->CreateBuffer(pDesc, pInitialData, ppBuffer); }
      HRESULT STDMETHODCALLTYPE CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture1D** ppTexture1D) override { return m_pDevice->CreateTexture1D(pDesc, pInitialData, ppTexture1D); }
      HRESULT STDMETHODCALLTYPE CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture2D** ppTexture2D) override {
        const HRESULT hr = m_pDevice->CreateTexture2D(pDesc, pInitialData, ppTexture2D);

        if (SUCCEEDED(hr) && ppTexture2D != nullptr && *ppTexture2D != nullptr) {
          tryShadowCreateTexture2D(*ppTexture2D, pDesc, pInitialData);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc, const D3D11_SUBRESOURCE_DATA* pInitialData, ID3D11Texture3D** ppTexture3D) override { return m_pDevice->CreateTexture3D(pDesc, pInitialData, ppTexture3D); }
      HRESULT STDMETHODCALLTYPE CreateShaderResourceView(ID3D11Resource* pResource, const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, ID3D11ShaderResourceView** ppSRView) override {
        const HRESULT hr = m_pDevice->CreateShaderResourceView(pResource, pDesc, ppSRView);

        if (SUCCEEDED(hr) && ppSRView != nullptr && *ppSRView != nullptr) {
          tryShadowCreateShaderResourceView(pResource, pDesc, *ppSRView);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D11Resource* pResource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, ID3D11UnorderedAccessView** ppUAView) override { return m_pDevice->CreateUnorderedAccessView(pResource, pDesc, ppUAView); }
      HRESULT STDMETHODCALLTYPE CreateRenderTargetView(ID3D11Resource* pResource, const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, ID3D11RenderTargetView** ppRTView) override {
        const HRESULT hr = m_pDevice->CreateRenderTargetView(pResource, pDesc, ppRTView);

        if (SUCCEEDED(hr) && ppRTView != nullptr && *ppRTView != nullptr) {
          tryShadowCreateRenderTargetView(pResource, pDesc, *ppRTView);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateDepthStencilView(ID3D11Resource* pResource, const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, ID3D11DepthStencilView** ppDepthStencilView) override {
        const HRESULT hr = m_pDevice->CreateDepthStencilView(pResource, pDesc, ppDepthStencilView);

        if (SUCCEEDED(hr) && ppDepthStencilView != nullptr && *ppDepthStencilView != nullptr) {
          tryShadowCreateDepthStencilView(pResource, pDesc, *ppDepthStencilView);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs, UINT NumElements, const void* pShaderBytecodeWithInputSignature, SIZE_T BytecodeLength, ID3D11InputLayout** ppInputLayout) override { return m_pDevice->CreateInputLayout(pInputElementDescs, NumElements, pShaderBytecodeWithInputSignature, BytecodeLength, ppInputLayout); }
      HRESULT STDMETHODCALLTYPE CreateVertexShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11VertexShader** ppVertexShader) override { return m_pDevice->CreateVertexShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppVertexShader); }
      HRESULT STDMETHODCALLTYPE CreateGeometryShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override { return m_pDevice->CreateGeometryShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppGeometryShader); }
      HRESULT STDMETHODCALLTYPE CreateGeometryShaderWithStreamOutput(const void* pShaderBytecode, SIZE_T BytecodeLength, const D3D11_SO_DECLARATION_ENTRY* pSODeclaration, UINT NumEntries, const UINT* pBufferStrides, UINT NumStrides, UINT RasterizedStream, ID3D11ClassLinkage* pClassLinkage, ID3D11GeometryShader** ppGeometryShader) override { return m_pDevice->CreateGeometryShaderWithStreamOutput(pShaderBytecode, BytecodeLength, pSODeclaration, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGeometryShader); }
      HRESULT STDMETHODCALLTYPE CreatePixelShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11PixelShader** ppPixelShader) override { return m_pDevice->CreatePixelShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppPixelShader); }
      HRESULT STDMETHODCALLTYPE CreateHullShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11HullShader** ppHullShader) override { return m_pDevice->CreateHullShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppHullShader); }
      HRESULT STDMETHODCALLTYPE CreateDomainShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11DomainShader** ppDomainShader) override { return m_pDevice->CreateDomainShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppDomainShader); }
      HRESULT STDMETHODCALLTYPE CreateComputeShader(const void* pShaderBytecode, SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage, ID3D11ComputeShader** ppComputeShader) override { return m_pDevice->CreateComputeShader(pShaderBytecode, BytecodeLength, pClassLinkage, ppComputeShader); }
      HRESULT STDMETHODCALLTYPE CreateClassLinkage(ID3D11ClassLinkage** ppLinkage) override { return m_pDevice->CreateClassLinkage(ppLinkage); }
      HRESULT STDMETHODCALLTYPE CreateBlendState(const D3D11_BLEND_DESC* pBlendStateDesc, ID3D11BlendState** ppBlendState) override {
        const HRESULT hr = m_pDevice->CreateBlendState(pBlendStateDesc, ppBlendState);

        if (SUCCEEDED(hr) && ppBlendState != nullptr && *ppBlendState != nullptr) {
          tryShadowCreateStateObject(Commands::ID3D11Bridge_CreateBlendState, *ppBlendState, pBlendStateDesc);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDepthStencilDesc, ID3D11DepthStencilState** ppDepthStencilState) override {
        const HRESULT hr = m_pDevice->CreateDepthStencilState(pDepthStencilDesc, ppDepthStencilState);

        if (SUCCEEDED(hr) && ppDepthStencilState != nullptr && *ppDepthStencilState != nullptr) {
          tryShadowCreateStateObject(Commands::ID3D11Bridge_CreateDepthStencilState, *ppDepthStencilState, pDepthStencilDesc);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateRasterizerState(const D3D11_RASTERIZER_DESC* pRasterizerDesc, ID3D11RasterizerState** ppRasterizerState) override {
        const HRESULT hr = m_pDevice->CreateRasterizerState(pRasterizerDesc, ppRasterizerState);

        if (SUCCEEDED(hr) && ppRasterizerState != nullptr && *ppRasterizerState != nullptr) {
          tryShadowCreateStateObject(Commands::ID3D11Bridge_CreateRasterizerState, *ppRasterizerState, pRasterizerDesc);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateSamplerState(const D3D11_SAMPLER_DESC* pSamplerDesc, ID3D11SamplerState** ppSamplerState) override {
        const HRESULT hr = m_pDevice->CreateSamplerState(pSamplerDesc, ppSamplerState);

        if (SUCCEEDED(hr) && ppSamplerState != nullptr && *ppSamplerState != nullptr) {
          tryShadowCreateStateObject(Commands::ID3D11Bridge_CreateSamplerState, *ppSamplerState, pSamplerDesc);
        }

        return hr;
      }
      HRESULT STDMETHODCALLTYPE CreateQuery(const D3D11_QUERY_DESC* pQueryDesc, ID3D11Query** ppQuery) override { return m_pDevice->CreateQuery(pQueryDesc, ppQuery); }
      HRESULT STDMETHODCALLTYPE CreatePredicate(const D3D11_QUERY_DESC* pPredicateDesc, ID3D11Predicate** ppPredicate) override { return m_pDevice->CreatePredicate(pPredicateDesc, ppPredicate); }
      HRESULT STDMETHODCALLTYPE CreateCounter(const D3D11_COUNTER_DESC* pCounterDesc, ID3D11Counter** ppCounter) override { return m_pDevice->CreateCounter(pCounterDesc, ppCounter); }
      HRESULT STDMETHODCALLTYPE CreateDeferredContext(UINT ContextFlags, ID3D11DeviceContext** ppDeferredContext) override { return m_pDevice->CreateDeferredContext(ContextFlags, ppDeferredContext); }
      HRESULT STDMETHODCALLTYPE OpenSharedResource(HANDLE hResource, REFIID ReturnedInterface, void** ppResource) override { return m_pDevice->OpenSharedResource(hResource, ReturnedInterface, ppResource); }
      HRESULT STDMETHODCALLTYPE CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport) override { return m_pDevice->CheckFormatSupport(Format, pFormatSupport); }
      HRESULT STDMETHODCALLTYPE CheckMultisampleQualityLevels(DXGI_FORMAT Format, UINT SampleCount, UINT* pNumQualityLevels) override { return m_pDevice->CheckMultisampleQualityLevels(Format, SampleCount, pNumQualityLevels); }
      void STDMETHODCALLTYPE CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo) override { m_pDevice->CheckCounterInfo(pCounterInfo); }
      HRESULT STDMETHODCALLTYPE CheckCounter(const D3D11_COUNTER_DESC* pDesc, D3D11_COUNTER_TYPE* pType, UINT* pActiveCounters, LPSTR szName, UINT* pNameLength, LPSTR szUnits, UINT* pUnitsLength, LPSTR szDescription, UINT* pDescriptionLength) override { return m_pDevice->CheckCounter(pDesc, pType, pActiveCounters, szName, pNameLength, szUnits, pUnitsLength, szDescription, pDescriptionLength); }
      HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D11_FEATURE Feature, void* pFeatureSupportData, UINT FeatureSupportDataSize) override { return m_pDevice->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize); }
      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override { return m_pDevice->GetPrivateData(guid, pDataSize, pData); }
      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) override { return m_pDevice->SetPrivateData(guid, DataSize, pData); }
      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) override { return m_pDevice->SetPrivateDataInterface(guid, pData); }
      D3D_FEATURE_LEVEL STDMETHODCALLTYPE GetFeatureLevel() override { return m_pDevice->GetFeatureLevel(); }
      UINT STDMETHODCALLTYPE GetCreationFlags() override { return m_pDevice->GetCreationFlags(); }
      HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override { return m_pDevice->GetDeviceRemovedReason(); }
      void STDMETHODCALLTYPE GetImmediateContext(ID3D11DeviceContext** ppImmediateContext) override { m_pDevice->GetImmediateContext(ppImmediateContext); }
      HRESULT STDMETHODCALLTYPE SetExceptionMode(UINT RaiseFlags) override { return m_pDevice->SetExceptionMode(RaiseFlags); }
      UINT STDMETHODCALLTYPE GetExceptionMode() override { return m_pDevice->GetExceptionMode(); }

      ID3D11Device* STDMETHODCALLTYPE GetUnderlyingDevice() override {
        if (m_pDevice != nullptr) {
          m_pDevice->AddRef();
        }
        return m_pDevice;
      }

    private:
      std::atomic<ULONG> m_refCount = 1;
      ID3D11Device* m_pDevice = nullptr;
    };

  }

  HRESULT createProxyD3D11Device(ID3D11Device** ppDevice) {
    if (ppDevice == nullptr || *ppDevice == nullptr) {
      return E_INVALIDARG;
    }

    ID3D11Device* pUnderlyingDevice = *ppDevice;
    *ppDevice = new D3D11DeviceProxy(pUnderlyingDevice);
    return S_OK;
  }

}