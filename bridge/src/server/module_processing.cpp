#pragma once

#include <windows.h>

#include "module_processing.h"

#include "remix_api.h"

#include "util_bridge_assert.h"
#include "util_modulecommand.h"

#include "log/log.h"

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>

using namespace Commands;

namespace {
  constexpr UINT kDefaultProxyAdapterIndex = UINT_MAX;
  IDXGIFactory1* gpDxgiFactory = nullptr;
  std::unordered_map<UINT, IDXGIAdapter1*> gDxgiAdapters;
  ID3D11Device* gpD3D11Device = nullptr;
  ID3D11DeviceContext* gpD3D11ImmediateContext = nullptr;
  std::unordered_map<UINT, ID3D11Texture2D*> gD3D11Textures2D;
  std::unordered_map<UINT, ID3D11RenderTargetView*> gD3D11RenderTargetViews;
  std::unordered_map<UINT, ID3D11ShaderResourceView*> gD3D11ShaderResourceViews;
  std::unordered_map<UINT, ID3D11DepthStencilView*> gD3D11DepthStencilViews;
  std::unordered_map<UINT, ID3D11BlendState*> gD3D11BlendStates;
  std::unordered_map<UINT, ID3D11DepthStencilState*> gD3D11DepthStencilStates;
  std::unordered_map<UINT, ID3D11RasterizerState*> gD3D11RasterizerStates;
  std::unordered_map<UINT, ID3D11SamplerState*> gD3D11SamplerStates;

  template <typename T>
  void clearTrackedD3D11Objects(std::unordered_map<UINT, T*>& objectMap) {
    for (auto& [handle, pObject] : objectMap) {
      if (pObject != nullptr) {
        pObject->Release();
      }
    }

    objectMap.clear();
  }

  template <typename T>
  void trackD3D11Object(std::unordered_map<UINT, T*>& objectMap, UINT handle, T* pObject) {
    const auto it = objectMap.find(handle);
    if (it != objectMap.end()) {
      if (it->second != nullptr) {
        it->second->Release();
      }

      if (pObject != nullptr) {
        it->second = pObject;
      } else {
        objectMap.erase(it);
      }

      return;
    }

    if (pObject != nullptr) {
      objectMap.emplace(handle, pObject);
    }
  }

  HRESULT ensureDxgiFactory() {
    if (gpDxgiFactory != nullptr) {
      return S_OK;
    }

    return CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&gpDxgiFactory));
  }

  IDXGIAdapter1* getDxgiAdapter(UINT adapterIndex, HRESULT& hr) {
    hr = ensureDxgiFactory();
    if (FAILED(hr)) {
      return nullptr;
    }

    const auto it = gDxgiAdapters.find(adapterIndex);
    if (it != gDxgiAdapters.end()) {
      hr = S_OK;
      return it->second;
    }

    IDXGIAdapter1* pAdapter = nullptr;
    hr = gpDxgiFactory->EnumAdapters1(adapterIndex, &pAdapter);
    if (SUCCEEDED(hr)) {
      gDxgiAdapters.emplace(adapterIndex, pAdapter);
    }

    return pAdapter;
  }

  void resetD3D11Device() {
    clearTrackedD3D11Objects(gD3D11SamplerStates);
    clearTrackedD3D11Objects(gD3D11RasterizerStates);
    clearTrackedD3D11Objects(gD3D11DepthStencilStates);
    clearTrackedD3D11Objects(gD3D11BlendStates);
    clearTrackedD3D11Objects(gD3D11DepthStencilViews);
    clearTrackedD3D11Objects(gD3D11ShaderResourceViews);
    clearTrackedD3D11Objects(gD3D11RenderTargetViews);
    clearTrackedD3D11Objects(gD3D11Textures2D);

    if (gpD3D11ImmediateContext != nullptr) {
      gpD3D11ImmediateContext->Release();
      gpD3D11ImmediateContext = nullptr;
    }

    if (gpD3D11Device != nullptr) {
      gpD3D11Device->Release();
      gpD3D11Device = nullptr;
    }
  }

  HRESULT createServerD3D11Device(UINT adapterIndex, D3D_DRIVER_TYPE driverType, UINT flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels, UINT featureLevels, UINT sdkVersion, D3D_FEATURE_LEVEL& featureLevel) {
    IDXGIAdapter1* pAdapter = nullptr;
    HRESULT hr = S_OK;

    if (adapterIndex != kDefaultProxyAdapterIndex) {
      pAdapter = getDxgiAdapter(adapterIndex, hr);
      if (FAILED(hr)) {
        return hr;
      }

      if (pAdapter != nullptr) {
        driverType = D3D_DRIVER_TYPE_UNKNOWN;
      }
    }

    resetD3D11Device();

    return D3D11CreateDevice(
      pAdapter,
      driverType,
      nullptr,
      flags,
      featureLevels > 0 ? pFeatureLevels : nullptr,
      featureLevels,
      sdkVersion,
      &gpD3D11Device,
      &featureLevel,
      &gpD3D11ImmediateContext);
  }
}

// Mapping between client and server pointer addresses
extern LPDIRECT3D9 gpD3D;

#define PULL(type, name) const auto& name = (type)ModuleBridge::get_data()
#define PULL_I(name) PULL(INT, name)
#define PULL_U(name) PULL(UINT, name)
#define PULL_D(name) PULL(DWORD, name)
#define PULL_H(name) PULL(HRESULT, name)
#define PULL_HND(name) \
            PULL_U(name); \
            assert(name != NULL)
#define PULL_DATA(size, name) \
            uint32_t name##_len = ModuleBridge::get_data((void**)&name); \
            assert(name##_len == 0 || size == name##_len)
#define PULL_OBJ(type, name) \
            type* name = nullptr; \
            PULL_DATA(sizeof(type), name)
#define CHECK_DATA_OFFSET (ModuleBridge::get_data_pos() == rpcHeader.dataOffset)
#define GET_HND(name) \
            const auto& name = rpcHeader.pHandle; \
            assert(name != NULL)
#define GET_HDR_VAL(name) \
            const DWORD& name = rpcHeader.pHandle;
#define GET_RES(name, map) \
            GET_HND(name##Handle); \
            const auto& name = map[name##Handle]; \
            assert(name != NULL)

// NOTE: MSDN states HWNDs are safe to cross x86-->x64 boundary, and that a truncating cast should be used: https://docs.microsoft.com/en-us/windows/win32/winprog64/interprocess-communication?redirectedfrom=MSDN
#define TRUNCATE_HANDLE(type, input) (type)(size_t)(input)

namespace {
  D3DPRESENT_PARAMETERS getPresParamFromRaw(const uint32_t* rawPresentationParameters) {
    D3DPRESENT_PARAMETERS presParam;
    // Set up presentation parameters. We can't just directly cast the structure because the hDeviceWindow
    // handle is 32-bit in the data coming in but 64-bit in the x64 version of the struct.
    presParam.BackBufferWidth = *reinterpret_cast<const UINT*>(rawPresentationParameters);
    presParam.BackBufferHeight = *reinterpret_cast<const UINT*>(rawPresentationParameters + 1);
    presParam.BackBufferFormat = *reinterpret_cast<const D3DFORMAT*>(rawPresentationParameters + 2);
    presParam.BackBufferCount = *reinterpret_cast<const UINT*>(rawPresentationParameters + 3);

    presParam.MultiSampleType = *reinterpret_cast<const D3DMULTISAMPLE_TYPE*>(rawPresentationParameters + 4);
    presParam.MultiSampleQuality = *reinterpret_cast<const DWORD*>(rawPresentationParameters + 5);

    presParam.SwapEffect = *reinterpret_cast<const D3DSWAPEFFECT*>(rawPresentationParameters + 6);
    presParam.hDeviceWindow = *reinterpret_cast<const HWND*>(rawPresentationParameters + 7);
    presParam.Windowed = *reinterpret_cast<const BOOL*>(rawPresentationParameters + 8);
    presParam.EnableAutoDepthStencil = *reinterpret_cast<const BOOL*>(rawPresentationParameters + 9);
    presParam.AutoDepthStencilFormat = *reinterpret_cast<const D3DFORMAT*>(rawPresentationParameters + 10);
    presParam.Flags = *reinterpret_cast<const DWORD*>(rawPresentationParameters + 11);

    presParam.FullScreen_RefreshRateInHz = *reinterpret_cast<const UINT*>(rawPresentationParameters + 12);
    presParam.PresentationInterval = (UINT) * reinterpret_cast<const UINT*>(rawPresentationParameters + 13);

    return presParam;
  }
}

void processModuleCommandQueue(std::atomic<bool>* const pbSignalEnd) {
  bool destroyReceived = false;
  while (RESULT_SUCCESS(ModuleBridge::waitForCommand(
    Commands::Bridge_Any, 0, pbSignalEnd))) {
    const Header rpcHeader = ModuleBridge::pop_front();
    PULL_U(currentUID);
#if defined(_DEBUG) || defined(DEBUGOPT)
    if (GlobalOptions::getLogServerCommands()) {
      Logger::info("Module Processing: " + toString(rpcHeader.command) + " UID: " + std::to_string(currentUID));
    }
#endif
    // The mother of all switch statements - every call in the D3D9 interface is mapped here...
    switch (rpcHeader.command) {
      /*
        * IDirect3D9 interface
        */
      case IDirect3D9Ex_QueryInterface:
        break;
      case IDirect3D9Ex_AddRef:
      {
        // The server controls its own device lifetime completely - no op
        break;
      }
      case IDirect3D9Ex_Destroy:
      {
        bridge_util::Logger::info("D3D9 Module destroyed.");
        destroyReceived = true;
        break;
      }
      case IDirect3D9Ex_RegisterSoftwareDevice:
        break;
      case IDirect3D9Ex_GetAdapterCount:
      {
        const auto cnt = gpD3D->GetAdapterCount();
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(cnt);
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterIdentifier:
      {
        PULL_U(Adapter);
        PULL_D(Flags);
        D3DADAPTER_IDENTIFIER9 pIdentifier;
        auto hresult = gpD3D->GetAdapterIdentifier(IN Adapter, IN Flags, OUT & pIdentifier);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(D3DADAPTER_IDENTIFIER9), &pIdentifier);
          }
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterModeCount:
      {
        PULL_U(Adapter);
        PULL(D3DFORMAT, Format);
        const auto cnt = gpD3D->GetAdapterModeCount(IN Adapter, IN Format);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(cnt);
        }
        break;
      }
      case IDirect3D9Ex_EnumAdapterModes:
      {
        PULL_U(Adapter);
        PULL(D3DFORMAT, Format);
        PULL_U(Mode);
        D3DDISPLAYMODE pMode;
        const auto hresult = gpD3D->EnumAdapterModes(IN Adapter, IN Format, IN Mode, OUT & pMode);
        BRIDGE_ASSERT_LOG(SUCCEEDED(hresult), "Issue checking Adapter compatibility with required format");
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(D3DDISPLAYMODE), &pMode);
          }
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterDisplayMode:
      {
        PULL_U(Adapter);
        D3DDISPLAYMODE pMode;
        const auto hresult = gpD3D->GetAdapterDisplayMode(IN Adapter, OUT & pMode);
        BRIDGE_ASSERT_LOG(SUCCEEDED(hresult), "Issue retrieving Adapter display mode");
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(D3DDISPLAYMODE), &pMode);
          }
        }
        break;
      }
      case IDirect3D9Ex_CheckDeviceType:
      {
        PULL_U(Adapter);
        PULL(D3DDEVTYPE, DevType);
        PULL(D3DFORMAT, AdapterFormat);
        PULL(D3DFORMAT, BackBufferFormat);
        PULL(BOOL, bWindowed);
        const auto hresult = gpD3D->CheckDeviceType(IN Adapter, IN DevType, IN AdapterFormat, IN BackBufferFormat, IN bWindowed);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
        }
        break;
      }
      case IDirect3D9Ex_CheckDeviceFormat:
      {
        PULL_U(Adapter);
        PULL(D3DDEVTYPE, DeviceType);
        PULL(D3DFORMAT, AdapterFormat);
        PULL_D(Usage);
        PULL(D3DRESOURCETYPE, RType);
        PULL(D3DFORMAT, CheckFormat);
        const auto hresult = gpD3D->CheckDeviceFormat(IN Adapter, IN DeviceType, IN AdapterFormat, IN Usage, IN RType, IN CheckFormat);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
        }
        break;
      }
      case IDirect3D9Ex_CheckDeviceMultiSampleType:
      {
        PULL_U(Adapter);
        PULL(D3DDEVTYPE, DeviceType);
        PULL(D3DFORMAT, SurfaceFormat);
        PULL(BOOL, Windowed);
        PULL(D3DMULTISAMPLE_TYPE, MultiSampleType);

        DWORD QualityLevels;
        const auto hresult = gpD3D->CheckDeviceMultiSampleType(IN Adapter, IN DeviceType, IN SurfaceFormat, IN Windowed, IN MultiSampleType, OUT & QualityLevels);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          c.send_data(QualityLevels);
        }
        break;
      }
      case IDirect3D9Ex_CheckDepthStencilMatch:
      {
        PULL_U(Adapter);
        PULL(D3DDEVTYPE, DeviceType);
        PULL(D3DFORMAT, AdapterFormat);
        PULL(D3DFORMAT, RenderTargetFormat);
        PULL(D3DFORMAT, DepthStencilFormat);
        const auto hresult = gpD3D->CheckDepthStencilMatch(IN Adapter, IN DeviceType, IN AdapterFormat, IN RenderTargetFormat, IN DepthStencilFormat);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
        }
        break;
      }
      case IDirect3D9Ex_CheckDeviceFormatConversion:
      {
        PULL_U(Adapter);
        PULL(D3DDEVTYPE, DeviceType);
        PULL(D3DFORMAT, SourceFormat);
        PULL(D3DFORMAT, TargetFormat);
        const auto hresult = gpD3D->CheckDeviceFormatConversion(IN Adapter, IN DeviceType, IN SourceFormat, IN TargetFormat);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
        }
        break;
      }
      case IDirect3D9Ex_GetDeviceCaps:
      {
        PULL_U(Adapter);
        PULL(D3DDEVTYPE, DeviceType);

        D3DCAPS9 pCaps;
        // Too many members in D3DCAPS so we just check the return value for now.
        const auto hresult = gpD3D->GetDeviceCaps(IN Adapter, IN DeviceType, OUT & pCaps);
        BRIDGE_ASSERT_LOG(SUCCEEDED(hresult), "Issue retrieving D3D9 device specific information");
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(D3DCAPS9), &pCaps);
          }
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterMonitor:
      {
        PULL_U(Adapter);
        HMONITOR hmonitor = gpD3D->GetAdapterMonitor(IN Adapter);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          // Truncate handle before sending back to client because it expects a 32-bit size handle
          c.send_data(TRUNCATE_HANDLE(uint32_t, hmonitor));
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterModeCountEx:
      {
        PULL_U(Adapter);
        D3DDISPLAYMODEFILTER modeFilter;
        PULL_DATA(sizeof(D3DDISPLAYMODEFILTER), modeFilter);
        const auto cnt = ((IDirect3D9Ex*) gpD3D)->GetAdapterModeCountEx(Adapter, &modeFilter);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(cnt);
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterLUID:
      {
        PULL_U(Adapter);
        LUID pLUID;
        HRESULT hresult = ((IDirect3D9Ex*) gpD3D)->GetAdapterLUID(Adapter, &pLUID);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(LUID), &pLUID);
          }
        }
        break;
      }

      case IDXGIFactory1_CreateFactory:
      {
        const auto hr = ensureDxgiFactory();
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case IDXGIFactory1_EnumAdapters1:
      {
        PULL_U(Adapter);
        HRESULT hr = S_OK;
        getDxgiAdapter(Adapter, hr);

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateDevice:
      {
        PULL_U(Adapter);
        PULL_U(DriverTypeRaw);
        PULL_U(Flags);
        PULL_U(FeatureLevels);
        PULL_U(SDKVersion);
        D3D_FEATURE_LEVEL* pFeatureLevelArray = nullptr;
        PULL_DATA(sizeof(D3D_FEATURE_LEVEL) * FeatureLevels, pFeatureLevelArray);

        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_9_1;
        const HRESULT hr = createServerD3D11Device(
          Adapter,
          static_cast<D3D_DRIVER_TYPE>(DriverTypeRaw),
          Flags,
          pFeatureLevelArray,
          FeatureLevels,
          SDKVersion,
          featureLevel);

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
          c.send_data(static_cast<UINT>(featureLevel));
        }
        break;
      }

      case ID3D11Bridge_CreateTexture2D:
      {
        PULL_U(TextureHandle);
        D3D11_TEXTURE2D_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_TEXTURE2D_DESC), pDesc);

        ID3D11Texture2D* pTexture = nullptr;
        HRESULT hr = gpD3D11Device != nullptr && pDesc != nullptr
          ? gpD3D11Device->CreateTexture2D(pDesc, nullptr, &pTexture)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11Textures2D, TextureHandle, pTexture);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateRenderTargetView:
      {
        PULL_U(ResourceHandle);
        PULL_U(RenderTargetViewHandle);
        D3D11_RENDER_TARGET_VIEW_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_RENDER_TARGET_VIEW_DESC), pDesc);

        const auto resourceIt = gD3D11Textures2D.find(ResourceHandle);
        ID3D11RenderTargetView* pRenderTargetView = nullptr;
        HRESULT hr = resourceIt != gD3D11Textures2D.end() && gpD3D11Device != nullptr
          ? gpD3D11Device->CreateRenderTargetView(resourceIt->second, pDesc, &pRenderTargetView)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11RenderTargetViews, RenderTargetViewHandle, pRenderTargetView);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateShaderResourceView:
      {
        PULL_U(ResourceHandle);
        PULL_U(ShaderResourceViewHandle);
        D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_SHADER_RESOURCE_VIEW_DESC), pDesc);

        const auto resourceIt = gD3D11Textures2D.find(ResourceHandle);
        ID3D11ShaderResourceView* pShaderResourceView = nullptr;
        HRESULT hr = resourceIt != gD3D11Textures2D.end() && gpD3D11Device != nullptr
          ? gpD3D11Device->CreateShaderResourceView(resourceIt->second, pDesc, &pShaderResourceView)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11ShaderResourceViews, ShaderResourceViewHandle, pShaderResourceView);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateDepthStencilView:
      {
        PULL_U(ResourceHandle);
        PULL_U(DepthStencilViewHandle);
        D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_DEPTH_STENCIL_VIEW_DESC), pDesc);

        const auto resourceIt = gD3D11Textures2D.find(ResourceHandle);
        ID3D11DepthStencilView* pDepthStencilView = nullptr;
        HRESULT hr = resourceIt != gD3D11Textures2D.end() && gpD3D11Device != nullptr
          ? gpD3D11Device->CreateDepthStencilView(resourceIt->second, pDesc, &pDepthStencilView)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11DepthStencilViews, DepthStencilViewHandle, pDepthStencilView);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateBlendState:
      {
        PULL_U(BlendStateHandle);
        D3D11_BLEND_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_BLEND_DESC), pDesc);

        ID3D11BlendState* pBlendState = nullptr;
        HRESULT hr = gpD3D11Device != nullptr && pDesc != nullptr
          ? gpD3D11Device->CreateBlendState(pDesc, &pBlendState)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11BlendStates, BlendStateHandle, pBlendState);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateDepthStencilState:
      {
        PULL_U(DepthStencilStateHandle);
        D3D11_DEPTH_STENCIL_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_DEPTH_STENCIL_DESC), pDesc);

        ID3D11DepthStencilState* pDepthStencilState = nullptr;
        HRESULT hr = gpD3D11Device != nullptr && pDesc != nullptr
          ? gpD3D11Device->CreateDepthStencilState(pDesc, &pDepthStencilState)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11DepthStencilStates, DepthStencilStateHandle, pDepthStencilState);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateRasterizerState:
      {
        PULL_U(RasterizerStateHandle);
        D3D11_RASTERIZER_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_RASTERIZER_DESC), pDesc);

        ID3D11RasterizerState* pRasterizerState = nullptr;
        HRESULT hr = gpD3D11Device != nullptr && pDesc != nullptr
          ? gpD3D11Device->CreateRasterizerState(pDesc, &pRasterizerState)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11RasterizerStates, RasterizerStateHandle, pRasterizerState);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case ID3D11Bridge_CreateSamplerState:
      {
        PULL_U(SamplerStateHandle);
        D3D11_SAMPLER_DESC* pDesc = nullptr;
        PULL_DATA(sizeof(D3D11_SAMPLER_DESC), pDesc);

        ID3D11SamplerState* pSamplerState = nullptr;
        HRESULT hr = gpD3D11Device != nullptr && pDesc != nullptr
          ? gpD3D11Device->CreateSamplerState(pDesc, &pSamplerState)
          : E_FAIL;

        if (SUCCEEDED(hr)) {
          trackD3D11Object(gD3D11SamplerStates, SamplerStateHandle, pSamplerState);
        }

        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hr);
        }
        break;
      }

      case IDirect3D9Ex_EnumAdapterModesEx:
      {
        PULL_U(Adapter);
        PULL_U(Mode);
        D3DDISPLAYMODEFILTER* pFilter = nullptr;
        PULL_DATA(sizeof(D3DDISPLAYMODEFILTER), pFilter);
        D3DDISPLAYMODEEX pMode;
        HRESULT hresult = ((IDirect3D9Ex*) gpD3D)->EnumAdapterModesEx(Adapter, pFilter, Mode, &pMode);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(D3DDISPLAYMODEEX), &pMode);
          }
        }
        break;
      }
      case IDirect3D9Ex_GetAdapterDisplayModeEx:
      {
        PULL_U(Adapter);
        D3DDISPLAYMODEEX* pMode = nullptr;
        PULL_DATA(sizeof(D3DDISPLAYMODEEX), pMode);
        D3DDISPLAYROTATION* pRotation = nullptr;
        PULL_DATA(sizeof(D3DDISPLAYROTATION), pRotation);
        HRESULT hresult = ((IDirect3D9Ex*) gpD3D)->GetAdapterDisplayModeEx(Adapter, pMode, pRotation);
        {
          ModuleServerCommand c(Commands::Bridge_Response, currentUID);
          c.send_data(hresult);
          if (SUCCEEDED(hresult)) {
            c.send_data(sizeof(D3DDISPLAYMODEEX), pMode);
            c.send_data(sizeof(D3DDISPLAYROTATION), pRotation);
          }
        }
        break;
      }
    }
  }
  // Check if we exited the command processing loop unexpectedly while the bridge is still enabled
  if (!destroyReceived && gbBridgeRunning) {
    Logger::info("The module command processing loop was exited unexpectedly, either due to timing out or some other command queue issue.");
  }
}
