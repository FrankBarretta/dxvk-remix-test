#include <array>
#include <csignal>
#include <cstdio>
#include <exception>

#include <windows.h>

#include "../dxgi/dxgi_adapter.h"

#include "../dxvk/dxvk_instance.h"

#include "d3d11_device.h"
#include "d3d11_enums.h"
#include "d3d11_interop.h"
#include "d3d11_trace.h"

#include "../util/util_env.h"
#include "../util/util_filesys.h"
#include "../util/util_once.h"

namespace dxvk {
  Logger Logger::s_instance("d3d11.log", LogLevel::None);
}

namespace {
  HMODULE g_d3d11Module = nullptr;
  std::terminate_handler g_previousTerminateHandler = nullptr;
  _purecall_handler g_previousPurecallHandler = nullptr;
  _invalid_parameter_handler g_previousInvalidParameterHandler = nullptr;
  LPTOP_LEVEL_EXCEPTION_FILTER g_previousUnhandledExceptionFilter = nullptr;
  bool g_enableEarlyTrace = false;

  struct ModuleAddressInfo {
    HMODULE moduleBase = nullptr;
    const char* moduleName = nullptr;
  };

  ModuleAddressInfo ModuleInfoFromAddress(const void* address, char* buffer, size_t bufferSize) {
    if (address == nullptr || buffer == nullptr || bufferSize == 0) {
      return {};
    }

    MEMORY_BASIC_INFORMATION memoryInfo = {};

    if (VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) == 0 || memoryInfo.AllocationBase == nullptr) {
      return {};
    }

    const DWORD pathLength = GetModuleFileNameA(
      static_cast<HMODULE>(memoryInfo.AllocationBase),
      buffer,
      static_cast<DWORD>(bufferSize));

    if (pathLength == 0 || pathLength >= bufferSize) {
      return { static_cast<HMODULE>(memoryInfo.AllocationBase), nullptr };
    }

    const char* lastSeparator = std::strrchr(buffer, '\\');
    return {
      static_cast<HMODULE>(memoryInfo.AllocationBase),
      lastSeparator != nullptr ? lastSeparator + 1 : buffer,
    };
  }

  void EarlyTraceImpl(const char* message) {
    char line[1024];
    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();
    const int lineLength = std::snprintf(line, sizeof(line), "[pid=%lu tid=%lu] %s\r\n", pid, tid, message);

    if (lineLength > 0)
      OutputDebugStringA(line);

    wchar_t path[MAX_PATH] = {};
    const DWORD pathLength = g_d3d11Module != nullptr
      ? GetModuleFileNameW(g_d3d11Module, path, MAX_PATH)
      : GetModuleFileNameW(nullptr, path, MAX_PATH);

    if (pathLength == 0 || pathLength >= MAX_PATH)
      return;

    wchar_t* lastSeparator = wcsrchr(path, L'\\');

    if (lastSeparator == nullptr)
      return;

    *(lastSeparator + 1) = L'\0';
    wcscat_s(path, L"d3d11-early.log");

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

  LONG WINAPI UnhandledExceptionLogger(EXCEPTION_POINTERS* exceptionInfo) {
    if (exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr) {
      char modulePath[MAX_PATH] = {};
      const void* exceptionAddress = exceptionInfo->ExceptionRecord->ExceptionAddress;
      const ModuleAddressInfo moduleInfo = ModuleInfoFromAddress(exceptionAddress, modulePath, sizeof(modulePath));
      const uintptr_t exceptionAddressValue = reinterpret_cast<uintptr_t>(exceptionAddress);
      const uintptr_t moduleBaseValue = reinterpret_cast<uintptr_t>(moduleInfo.moduleBase);
      const uintptr_t moduleOffset = moduleBaseValue != 0u && exceptionAddressValue >= moduleBaseValue
        ? exceptionAddressValue - moduleBaseValue
        : 0u;
      char message[384];

      if (moduleInfo.moduleName != nullptr) {
        std::snprintf(message,
          sizeof(message),
          "Unhandled exception code=0x%08lX address=%p module=%s base=%p rva=0x%llX",
          static_cast<unsigned long>(exceptionInfo->ExceptionRecord->ExceptionCode),
          exceptionAddress,
          moduleInfo.moduleName,
          moduleInfo.moduleBase,
          static_cast<unsigned long long>(moduleOffset));
      } else {
        std::snprintf(message,
          sizeof(message),
          "Unhandled exception code=0x%08lX address=%p base=%p",
          static_cast<unsigned long>(exceptionInfo->ExceptionRecord->ExceptionCode),
          exceptionAddress,
          moduleInfo.moduleBase);
      }

      EarlyTraceImpl(message);
    } else {
      EarlyTraceImpl("Unhandled exception with no exception record");
    }

    return g_previousUnhandledExceptionFilter != nullptr
      ? g_previousUnhandledExceptionFilter(exceptionInfo)
      : EXCEPTION_CONTINUE_SEARCH;
  }

  void __cdecl TerminateLogger() {
    EarlyTraceImpl("std::terminate called");

    if (g_previousTerminateHandler != nullptr) {
      auto handler = g_previousTerminateHandler;
      g_previousTerminateHandler = nullptr;
      std::set_terminate(handler);
      handler();
    }

    std::abort();
  }

  void __cdecl PurecallLogger() {
    EarlyTraceImpl("_purecall invoked");

    if (g_previousPurecallHandler != nullptr) {
      auto handler = g_previousPurecallHandler;
      g_previousPurecallHandler = nullptr;
      _set_purecall_handler(handler);
      handler();
      return;
    }

    std::abort();
  }

  void __cdecl InvalidParameterLogger(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    EarlyTraceImpl("invalid parameter handler invoked");

    if (g_previousInvalidParameterHandler != nullptr) {
      auto handler = g_previousInvalidParameterHandler;
      g_previousInvalidParameterHandler = nullptr;
      _set_invalid_parameter_handler(handler);
      handler(nullptr, nullptr, nullptr, 0, 0);
      return;
    }

    std::abort();
  }

  void __cdecl SignalLogger(int signalValue) {
    char message[128];
    std::snprintf(message, sizeof(message), "signal handler invoked signal=%d", signalValue);
    EarlyTraceImpl(message);
    std::signal(signalValue, SIG_DFL);
    std::raise(signalValue);
  }

  void InstallCrashDiagnostics() {
    static bool installed = false;

    if (installed)
      return;

    installed = true;
    EarlyTraceImpl("Installing crash diagnostics");

    g_previousTerminateHandler = std::set_terminate(TerminateLogger);
    g_previousPurecallHandler = _set_purecall_handler(PurecallLogger);
    g_previousInvalidParameterHandler = _set_invalid_parameter_handler(InvalidParameterLogger);
    g_previousUnhandledExceptionFilter = SetUnhandledExceptionFilter(UnhandledExceptionLogger);
    std::signal(SIGABRT, SignalLogger);
    std::signal(SIGILL, SignalLogger);
    std::signal(SIGINT, SignalLogger);
    std::signal(SIGSEGV, SignalLogger);
    std::signal(SIGTERM, SignalLogger);
  }

  int SehFilter(const char* scope, DWORD code, EXCEPTION_POINTERS* exceptionInfo) {
    char message[320];
    const void* exceptionAddress = exceptionInfo != nullptr && exceptionInfo->ExceptionRecord != nullptr
      ? exceptionInfo->ExceptionRecord->ExceptionAddress
      : nullptr;
    std::snprintf(message, sizeof(message), "%s failed with SEH 0x%08lX at %p", scope, static_cast<unsigned long>(code), exceptionAddress);
    EarlyTraceImpl(message);
    return EXCEPTION_EXECUTE_HANDLER;
  }
}

namespace dxvk {
}
  
extern "C" {
  using namespace dxvk;

  BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
      g_d3d11Module = hinstDLL;
      g_enableEarlyTrace = env::getEnvVar("DXVK_D3D11_EARLY_TRACE") == "1";
      DisableThreadLibraryCalls(hinstDLL);
      EarlyTraceImpl("DllMain DLL_PROCESS_ATTACH");
    }

    return TRUE;
  }
  
  DLLEXPORT HRESULT __stdcall D3D11CoreCreateDevice(
          IDXGIFactory*       pFactory,
          IDXGIAdapter*       pAdapter,
          UINT                Flags,
    const D3D_FEATURE_LEVEL*  pFeatureLevels,
          UINT                FeatureLevels,
          ID3D11Device**      ppDevice) {
    InitReturnPtr(ppDevice);
        EarlyTraceImpl("D3D11CoreCreateDevice enter");

    Rc<DxvkAdapter>  dxvkAdapter;
    Rc<DxvkInstance> dxvkInstance;

    Com<IDXGIDXVKAdapter> dxgiVkAdapter;
    
    // Try to find the corresponding Vulkan device for the DXGI adapter
    if (SUCCEEDED(pAdapter->QueryInterface(__uuidof(IDXGIDXVKAdapter), reinterpret_cast<void**>(&dxgiVkAdapter)))) {
      dxvkAdapter  = dxgiVkAdapter->GetDXVKAdapter();
      dxvkInstance = dxgiVkAdapter->GetDXVKInstance();
    } else {
      Logger::warn("D3D11CoreCreateDevice: Adapter is not a DXVK adapter");
      DXGI_ADAPTER_DESC desc;
      pAdapter->GetDesc(&desc);

      dxvkInstance = new DxvkInstance();
      dxvkAdapter  = dxvkInstance->findAdapterByLuid(&desc.AdapterLuid);

      if (dxvkAdapter == nullptr)
        dxvkAdapter = dxvkInstance->findAdapterByDeviceId(desc.VendorId, desc.DeviceId);
      
      if (dxvkAdapter == nullptr)
        dxvkAdapter = dxvkInstance->enumAdapters(0);

      if (dxvkAdapter == nullptr)
        return E_FAIL;
    }
    
    // Feature levels to probe if the
    // application does not specify any.
    std::array<D3D_FEATURE_LEVEL, 6> defaultFeatureLevels = {
      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,
      D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1,
    };
    
    if (pFeatureLevels == nullptr || FeatureLevels == 0) {
      pFeatureLevels = defaultFeatureLevels.data();
      FeatureLevels  = defaultFeatureLevels.size();
    }
    
    // Find the highest feature level supported by the device.
    // This works because the feature level array is ordered.
    UINT flId;

    for (flId = 0 ; flId < FeatureLevels; flId++) {
      Logger::info(str::format("D3D11CoreCreateDevice: Probing ", pFeatureLevels[flId]));
      
      if (D3D11Device::CheckFeatureLevelSupport(dxvkInstance, dxvkAdapter, pFeatureLevels[flId]))
        break;
    }
    
    if (flId == FeatureLevels) {
      Logger::err("D3D11CoreCreateDevice: Requested feature level not supported");
      return E_INVALIDARG;
    }
    
    // Try to create the device with the given parameters.
    const D3D_FEATURE_LEVEL fl = pFeatureLevels[flId];
    
    try {
      Logger::info(str::format("D3D11CoreCreateDevice: Using feature level ", fl));
      EarlyTraceImpl("D3D11CoreCreateDevice creating D3D11DXGIDevice");
      Com<D3D11DXGIDevice> device = new D3D11DXGIDevice(
        pAdapter, dxvkInstance, dxvkAdapter, fl, Flags);

      EarlyTraceImpl("D3D11CoreCreateDevice created D3D11DXGIDevice");
      
      return device->QueryInterface(
        __uuidof(ID3D11Device),
        reinterpret_cast<void**>(ppDevice));
    } catch (const DxvkError& e) {
      EarlyTraceImpl(str::format("D3D11CoreCreateDevice threw DxvkError: ", e.message()).c_str());
      Logger::err("D3D11CoreCreateDevice: Failed to create D3D11 device");
      return E_FAIL;
    }
  }
  
  
  static HRESULT D3D11InternalCreateDeviceAndSwapChain(
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
    InitReturnPtr(ppDevice);
    InitReturnPtr(ppSwapChain);
    InitReturnPtr(ppImmediateContext);

    EarlyTraceImpl("D3D11InternalCreateDeviceAndSwapChain enter");

    ONCE(
      const auto exePath = env::getExePath();
      const auto exeDir = std::filesystem::path(exePath).parent_path();
      EarlyTraceImpl(str::format("D3D11InternalCreateDeviceAndSwapChain init root ", exeDir.string()).c_str());
      util::RtxFileSys::init(exeDir.string());
      Logger::initRtxLog();
          Logger::info("D3D11 RTX logger initialized");
      util::RtxFileSys::print();
      InstallCrashDiagnostics();
      EarlyTraceImpl("D3D11InternalCreateDeviceAndSwapChain init complete");
    );

    if (pFeatureLevel)
      *pFeatureLevel = D3D_FEATURE_LEVEL(0);

    HRESULT hr;

    Com<IDXGIFactory> dxgiFactory = nullptr;
    Com<IDXGIAdapter> dxgiAdapter = pAdapter;
    Com<ID3D11Device> device      = nullptr;
    
    if (ppSwapChain && !pSwapChainDesc)
      return E_INVALIDARG;
    
    if (!pAdapter) {
      // We'll treat everything as hardware, even if the
      // Vulkan device is actually a software device.
      if (DriverType != D3D_DRIVER_TYPE_HARDWARE)
        Logger::warn("D3D11CreateDevice: Unsupported driver type");
      
      // We'll use the first adapter returned by a DXGI factory
      hr = CreateDXGIFactory1(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory));

      if (FAILED(hr)) {
        Logger::err("D3D11CreateDevice: Failed to create a DXGI factory");
        return hr;
      }

      hr = dxgiFactory->EnumAdapters(0, &dxgiAdapter);

      if (FAILED(hr)) {
        Logger::err("D3D11CreateDevice: No default adapter available");
        return hr;
      }
    } else {
      // We should be able to query the DXGI factory from the adapter
      if (FAILED(dxgiAdapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&dxgiFactory)))) {
        Logger::err("D3D11CreateDevice: Failed to query DXGI factory from DXGI adapter");
        return E_INVALIDARG;
      }
      
      // In theory we could ignore these, but the Microsoft docs explicitly
      // state that we need to return E_INVALIDARG in case the arguments are
      // invalid. Both the driver type and software parameter can only be
      // set if the adapter itself is unspecified.
      // See: https://msdn.microsoft.com/en-us/library/windows/desktop/ff476082(v=vs.85).aspx
      if (DriverType != D3D_DRIVER_TYPE_UNKNOWN || Software)
        return E_INVALIDARG;
    }
    
    // Create the actual device
    hr = D3D11CoreCreateDevice(
      dxgiFactory.ptr(), dxgiAdapter.ptr(),
      Flags, pFeatureLevels, FeatureLevels,
      &device);

    char hrMessage[128];
    std::snprintf(hrMessage, sizeof(hrMessage), "D3D11InternalCreateDeviceAndSwapChain D3D11CoreCreateDevice hr=0x%08lX", static_cast<unsigned long>(hr));
    EarlyTraceImpl(hrMessage);
    
    if (FAILED(hr))
      return hr;
    
    // Create the swap chain, if requested
    if (ppSwapChain) {
      DXGI_SWAP_CHAIN_DESC desc = *pSwapChainDesc;
      hr = dxgiFactory->CreateSwapChain(device.ptr(), &desc, ppSwapChain);

      if (FAILED(hr)) {
        Logger::err("D3D11CreateDevice: Failed to create swap chain");
        return hr;
      }
    }
    
    // Write back whatever info the application requested
    if (pFeatureLevel)
      *pFeatureLevel = device->GetFeatureLevel();
    
    if (ppDevice)
      *ppDevice = device.ref();
    
    if (ppImmediateContext)
      device->GetImmediateContext(ppImmediateContext);

    // If we were unable to write back the device and the
    // swap chain, the application has no way of working
    // with the device so we should report S_FALSE here.
    if (!ppDevice && !ppImmediateContext && !ppSwapChain)
      return S_FALSE;

    EarlyTraceImpl("D3D11InternalCreateDeviceAndSwapChain success");
    
    return S_OK;
  }
  

  DLLEXPORT HRESULT __stdcall D3D11CreateDevice(
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
            EarlyTraceImpl("D3D11CreateDevice enter");

    __try {
      return D3D11InternalCreateDeviceAndSwapChain(
        pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion,
        nullptr, nullptr,
        ppDevice, pFeatureLevel, ppImmediateContext);
    } __except (SehFilter("D3D11CreateDevice", GetExceptionCode(), GetExceptionInformation())) {
      return E_FAIL;
    }
  }
  
  
  DLLEXPORT HRESULT __stdcall D3D11CreateDeviceAndSwapChain(
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
            EarlyTraceImpl("D3D11CreateDeviceAndSwapChain enter");

    __try {
      return D3D11InternalCreateDeviceAndSwapChain(
        pAdapter, DriverType, Software, Flags,
        pFeatureLevels, FeatureLevels, SDKVersion,
        pSwapChainDesc, ppSwapChain,
        ppDevice, pFeatureLevel, ppImmediateContext);
    } __except (SehFilter("D3D11CreateDeviceAndSwapChain", GetExceptionCode(), GetExceptionInformation())) {
      return E_FAIL;
    }
  }
  

  DLLEXPORT HRESULT __stdcall D3D11On12CreateDevice(
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
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::err("D3D11On12CreateDevice: Not implemented");

    return E_NOTIMPL;
  }

}
