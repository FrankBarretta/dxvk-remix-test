#include "dxgi_factory.h"
#include "dxgi_include.h"
#include "dxgi_trace.h"

#include "../util/util_env.h"
#include "../util/util_filesys.h"
#include "../util/util_once.h"

namespace dxvk {
  
  Logger Logger::s_instance("dxgi.log", LogLevel::None);
  
  HRESULT createDxgiFactory(UINT Flags, REFIID riid, void **ppFactory) {
    DxgiEarlyTrace("createDxgiFactory enter");

    ONCE(
      const auto exePath = env::getExePath();
      const auto exeDir = std::filesystem::path(exePath).parent_path();
      DxgiEarlyTrace(str::format("createDxgiFactory init root ", exeDir.string()).c_str());

      if (!util::RtxFileSys::isInitialized())
        util::RtxFileSys::init(exeDir.string());

      Logger::initRtxLog();
        Logger::info("DXGI RTX logger initialized");

      if (util::RtxFileSys::isInitialized())
        util::RtxFileSys::print();
    );

    try {
      Com<DxgiFactory> factory = new DxgiFactory(Flags);
      HRESULT hr = factory->QueryInterface(riid, ppFactory);

      char hrMessage[128];
      std::snprintf(hrMessage, sizeof(hrMessage), "createDxgiFactory QueryInterface hr=0x%08lX", static_cast<unsigned long>(hr));
      DxgiEarlyTrace(hrMessage);

      if (FAILED(hr))
        return hr;
      
      DxgiEarlyTrace("createDxgiFactory success");
      return S_OK;
    } catch (const DxvkError& e) {
      DxgiEarlyTrace(str::format("createDxgiFactory DxvkError: ", e.message()).c_str());
      Logger::err(e.message());
      return E_FAIL;
    }
  }
}

extern "C" {
  BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
      DisableThreadLibraryCalls(hinstDLL);
      dxvk::DxgiEarlyTrace("DllMain DLL_PROCESS_ATTACH");
    }

    return TRUE;
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid, void **ppFactory) {
    dxvk::DxgiEarlyTrace("CreateDXGIFactory2 enter");
    dxvk::Logger::warn("CreateDXGIFactory2: Ignoring flags");
    return dxvk::createDxgiFactory(Flags, riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    dxvk::DxgiEarlyTrace("CreateDXGIFactory1 enter");
    return dxvk::createDxgiFactory(0, riid, ppFactory);
  }
  
  DLLEXPORT HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **ppFactory) {
    dxvk::DxgiEarlyTrace("CreateDXGIFactory enter");
    return dxvk::createDxgiFactory(0, riid, ppFactory);
  }

  DLLEXPORT HRESULT __stdcall DXGIDeclareAdapterRemovalSupport() {
    static bool enabled = false;

    if (std::exchange(enabled, true))
      return 0x887a0036; // DXGI_ERROR_ALREADY_EXISTS;

    dxvk::Logger::warn("DXGIDeclareAdapterRemovalSupport: Stub");
    return S_OK;
  }

  DLLEXPORT HRESULT __stdcall DXGIGetDebugInterface1(UINT Flags, REFIID riid, void **ppDebug) {
    static bool errorShown = false;

    if (!std::exchange(errorShown, true))
      dxvk::Logger::warn("DXGIGetDebugInterface1: Stub");

    return E_NOINTERFACE;
  }

}
