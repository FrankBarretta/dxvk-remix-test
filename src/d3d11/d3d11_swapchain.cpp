#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_swapchain.h"
#include "d3d11_trace.h"

#include <mutex>
#include <unordered_map>

#include "../dxvk/dxvk_objects.h"
#include "../dxvk/imgui/dxvk_imgui.h"
#include "../dxvk/rtx_render/rtx_option.h"
#include "../dxvk/rtx_render/rtx_bridge_message_channel.h"
#include "../util/util_string.h"

namespace dxvk {

  namespace {

    bool ShouldTracePresentFrame(uint64_t frameId) {
      return frameId < 8;
    }

    struct D3D11WindowData {
      bool unicode;
      WNDPROC proc;
      D3D11SwapChain* swapchain;
    };

    std::recursive_mutex g_d3d11WindowProcMapMutex;
    std::unordered_map<HWND, D3D11WindowData> g_d3d11WindowProcMap;

    template <typename T, typename J, typename ... Args>
    auto CallCharsetFunction(T unicode, J ascii, bool isUnicode, Args... args) {
      return isUnicode
        ? unicode(args...)
        : ascii(args...);
    }

    bool IsInputMessage(UINT message) {
      switch (message) {
        case WM_INPUT:
        case WM_CHAR:
        case WM_SYSCHAR:
        case WM_UNICHAR:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_MOUSEMOVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
          return true;
        default:
          return false;
      }
    }

    LRESULT CALLBACK D3D11WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void ResetWindowProc(HWND window) {
      std::lock_guard<std::recursive_mutex> lock(g_d3d11WindowProcMapMutex);

      auto it = g_d3d11WindowProcMap.find(window);
      if (it == g_d3d11WindowProcMap.end())
        return;

      auto proc = reinterpret_cast<WNDPROC>(
        CallCharsetFunction(
          GetWindowLongPtrW, GetWindowLongPtrA, it->second.unicode,
          window, GWLP_WNDPROC));

      if (proc == D3D11WindowProc && it->second.proc != nullptr) {
        CallCharsetFunction(
          SetWindowLongPtrW, SetWindowLongPtrA, it->second.unicode,
          window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.proc));
      }

      g_d3d11WindowProcMap.erase(window);
    }

    void HookWindowProc(D3D11SwapChain* swapchain, HWND window) {
      if (window == nullptr)
        return;

      std::lock_guard<std::recursive_mutex> lock(g_d3d11WindowProcMapMutex);

      ResetWindowProc(window);

      D3D11WindowData windowData;
      windowData.unicode = IsWindowUnicode(window);
      windowData.proc = reinterpret_cast<WNDPROC>(
        CallCharsetFunction(
          SetWindowLongPtrW, SetWindowLongPtrA, windowData.unicode,
          window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(D3D11WindowProc)));
      windowData.swapchain = swapchain;
      g_d3d11WindowProcMap[window] = windowData;

      Logger::info(str::format("D3D11: Hooked window proc for window ", window, ", previous proc=", reinterpret_cast<const void*>(windowData.proc)));

      if (windowData.proc == nullptr) {
        Logger::info(str::format("D3D11: No winproc detected, initiating bridge message channel for: ", window));

        if (BridgeMessageChannel::get().init(window, D3D11WindowProc)) {
          auto& gui = swapchain->GetDxvkDevice()->getCommon()->getImgui();
          gui.switchMenu(RtxOptions::showUI(), true);
        } else {
          Logger::err("D3D11: Unable to init bridge message channel. Input capture may not work!");
        }
      }
    }

    LRESULT CALLBACK D3D11WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
      D3D11WindowData windowData = {};

      {
        std::lock_guard<std::recursive_mutex> lock(g_d3d11WindowProcMapMutex);

        auto it = g_d3d11WindowProcMap.find(window);
        if (it == g_d3d11WindowProcMap.end())
          return 0;

        windowData = it->second;
      }

      if (message == WM_DESTROY || message == WM_NCDESTROY)
        ResetWindowProc(window);

      if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP) {
        if (wParam == VK_MENU || wParam == 'X') {
          Logger::info(str::format("D3D11WindowProc: key message=", message, ", vk=", wParam, ", hwnd=", window));
        }
      }

      auto& gui = windowData.swapchain->GetDxvkDevice()->getCommon()->getImgui();
      if (gui.isInit())
        gui.wndProcHandler(window, message, wParam, lParam);

      if (RtxOptions::showUI() != UIType::None
       && RtxOptions::blockInputToGameInUI()
       && IsInputMessage(message)) {
        return 0;
      }

      if (windowData.proc != nullptr) {
        const bool unicode = windowData.proc
          ? windowData.unicode
          : IsWindowUnicode(window);

        return CallCharsetFunction(
          CallWindowProcW, CallWindowProcA, unicode,
          windowData.proc, window, message, wParam, lParam);
      }

      return 0;
    }

  }

  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  D3D11SwapChain::D3D11SwapChain(
          D3D11DXGIDevice*        pContainer,
          D3D11Device*            pDevice,
          HWND                    hWnd,
    const DXGI_SWAP_CHAIN_DESC1*  pDesc)
  : m_dxgiDevice(pContainer),
    m_parent    (pDevice),
    m_window    (hWnd),
    m_desc      (*pDesc),
    m_device    (pDevice->GetDXVKDevice()),
    m_context   (m_device->createContext()),
    m_frameLatencyCap(pDevice->GetOptions()->maxFrameLatency) {
    CreateFrameLatencyEvent();

    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();
    
    CreateBackBuffer();
    CreateBlitter();
    CreateHud();

    HookWindowProc(this, m_window);
  }


  D3D11SwapChain::~D3D11SwapChain() {
    ResetWindowProc(m_window);

    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();
    
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();

    DestroyFrameLatencyEvent();
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::QueryInterface(
          REFIID                  riid,
          void**                  ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    InitReturnPtr(ppvObject);

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDXGIVkSwapChain)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D11SwapChain::QueryInterface: Unknown interface query");
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDesc(
          DXGI_SWAP_CHAIN_DESC1*    pDesc) {
    *pDesc = m_desc;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetAdapter(
          REFIID                    riid,
          void**                    ppvObject) {
    return m_dxgiDevice->GetParent(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetDevice(
          REFIID                    riid,
          void**                    ppDevice) {
    return m_dxgiDevice->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::GetImage(
          UINT                      BufferId,
          REFIID                    riid,
          void**                    ppBuffer) {
    InitReturnPtr(ppBuffer);

    if (BufferId > 0) {
      Logger::err("D3D11: GetImage: BufferId > 0 not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

    return m_backBuffer->QueryInterface(riid, ppBuffer);
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetImageIndex() {
    return 0;
  }


  UINT STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatency() {
    return m_frameLatency;
  }


  HANDLE STDMETHODCALLTYPE D3D11SwapChain::GetFrameLatencyEvent() {
    return m_frameLatencyEvent;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::ChangeProperties(
    const DXGI_SWAP_CHAIN_DESC1*  pDesc) {

    m_dirty |= m_desc.Format      != pDesc->Format
            || m_desc.Width       != pDesc->Width
            || m_desc.Height      != pDesc->Height
            || m_desc.BufferCount != pDesc->BufferCount
            || m_desc.Flags       != pDesc->Flags;

    m_desc = *pDesc;
    CreateBackBuffer();
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetPresentRegion(
    const RECT*                     pRegion) {
    // TODO implement
    return E_NOTIMPL;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetGammaControl(
          UINT                      NumControlPoints,
    const DXGI_RGB*                 pControlPoints) {
    bool isIdentity = true;

    if (NumControlPoints > 1) {
      std::array<DxvkGammaCp, 1025> cp;

      if (NumControlPoints > cp.size())
        return E_INVALIDARG;
      
      for (uint32_t i = 0; i < NumControlPoints; i++) {
        uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

        cp[i].r = MapGammaControlPoint(pControlPoints[i].Red);
        cp[i].g = MapGammaControlPoint(pControlPoints[i].Green);
        cp[i].b = MapGammaControlPoint(pControlPoints[i].Blue);
        cp[i].a = 0;

        isIdentity &= cp[i].r == identity
                   && cp[i].g == identity
                   && cp[i].b == identity;
      }

      if (!isIdentity)
        m_blitter->setGammaRamp(NumControlPoints, cp.data());
    }

    if (isIdentity)
      m_blitter->setGammaRamp(0, nullptr);

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::SetFrameLatency(
          UINT                      MaxLatency) {
    if (MaxLatency == 0 || MaxLatency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
      return DXGI_ERROR_INVALID_CALL;

    if (m_frameLatencyEvent) {
      // Windows DXGI does not seem to handle the case where the new maximum
      // latency is less than the current value, and some games relying on
      // this behaviour will hang if we attempt to decrement the semaphore.
      // Thus, only increment the semaphore as necessary.
      if (MaxLatency > m_frameLatency)
        ReleaseSemaphore(m_frameLatencyEvent, MaxLatency - m_frameLatency, nullptr);
    }

    m_frameLatency = MaxLatency;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D11SwapChain::Present(
          UINT                      SyncInterval,
          UINT                      PresentFlags,
    const DXGI_PRESENT_PARAMETERS*  pPresentParameters) {
    std::lock_guard<std::mutex> lock(m_presentMutex);

    auto options = m_parent->GetOptions();

    if (options->syncInterval >= 0)
      SyncInterval = options->syncInterval;

    if (RtxOptions::enableVsyncState == EnableVsync::WaitingForImplicitSwapchain)
      RtxOptions::enableVsyncState = SyncInterval != 0 ? EnableVsync::On : EnableVsync::Off;

    if (!(PresentFlags & DXGI_PRESENT_TEST)) {
      bool vsync = SyncInterval != 0;

      m_dirty |= vsync != m_vsync;
      m_vsync  = vsync;
    }

    if (m_presenter == nullptr)
      CreatePresenter();

    HRESULT hr = S_OK;

    if (!m_presenter->hasSwapChain()) {
      RecreateSwapChain(m_vsync);
      m_dirty = false;
    }

    if (!m_presenter->hasSwapChain())
      hr = DXGI_STATUS_OCCLUDED;

    if (m_device->getDeviceStatus() != VK_SUCCESS)
      hr = DXGI_ERROR_DEVICE_RESET;

    if ((PresentFlags & DXGI_PRESENT_TEST) || hr != S_OK)
      return hr;

    if (std::exchange(m_dirty, false))
      RecreateSwapChain(m_vsync);
    
    try {
      PresentImage(SyncInterval);
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      hr = E_FAIL;
    }

    return hr;
  }


  void STDMETHODCALLTYPE D3D11SwapChain::NotifyModeChange(
          BOOL                      Windowed,
    const DXGI_MODE_DESC*           pDisplayMode) {
    if (Windowed || !pDisplayMode) {
      // Display modes aren't meaningful in windowed mode
      m_displayRefreshRate = 0.0;
    } else {
      DXGI_RATIONAL rate = pDisplayMode->RefreshRate;
      m_displayRefreshRate = double(rate.Numerator) / double(rate.Denominator);
    }

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);
  }


  HRESULT D3D11SwapChain::PresentImage(UINT SyncInterval) {
    const bool tracePresent = ShouldTracePresentFrame(m_frameId);

    if (tracePresent)
      D3D11EarlyTrace("D3D11SwapChain::PresentImage enter");

    Com<ID3D11DeviceContext> deviceContext = nullptr;
    m_parent->GetImmediateContext(&deviceContext);

    if (tracePresent)
      D3D11EarlyTrace("D3D11SwapChain::PresentImage got immediate context");

    auto immediateContext = static_cast<D3D11ImmediateContext*>(deviceContext.ptr());
    D3D10DeviceLock contextLock = immediateContext->LockContext();

    m_parent->RTX().ResetScreenResolution(immediateContext,
      std::max(m_desc.Width, 1u),
      std::max(m_desc.Height, 1u));

    if (tracePresent)
      D3D11EarlyTrace("D3D11SwapChain::PresentImage after ResetScreenResolution");

    m_parent->RTX().EndFrame(immediateContext, m_swapImage);

    if (tracePresent)
      D3D11EarlyTrace("D3D11SwapChain::PresentImage after EndFrame");

    // Flush pending rendering commands before
    immediateContext->Flush();

    if (tracePresent)
      D3D11EarlyTrace("D3D11SwapChain::PresentImage after immediate Flush");

    // Bump our frame id.
    ++m_frameId;
    
    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      SynchronizePresent();

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage after SynchronizePresent");

      if (!m_presenter->hasSwapChain())
        return DXGI_STATUS_OCCLUDED;

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;

      uint32_t imageIndex = 0;

      VkResult status = m_presenter->acquireNextImage(sync, imageIndex);

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage acquireNextImage complete");

      while (status != VK_SUCCESS) {
        RecreateSwapChain(m_vsync);

        if (!m_presenter->hasSwapChain())
          return DXGI_STATUS_OCCLUDED;
        
        info = m_presenter->info();
        status = m_presenter->acquireNextImage(sync, imageIndex);

        if (status == VK_SUBOPTIMAL_KHR)
          break;
      }

      // Resolve back buffer if it is multisampled. We
      // only have to do it only for the first frame.
      m_context->beginRecording(
        m_device->createCommandList());

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage recording begun");
      
      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), VkRect2D(),
        m_swapImageView, VkRect2D());

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage blitter presentImage complete");

      if (m_hud != nullptr)
        m_hud->render(m_context, info.format, info.imageExtent);

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage HUD render complete");

      auto& gui = m_device->getCommon()->getImgui();
      gui.render(m_window, m_context, info.format, info.imageExtent, m_vsync);

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage ImGUI render complete");

      if (m_parent->GetOptions()->enableRemix) {
        D3D11EarlyTrace("D3D11SwapChain::PresentImage remix skipping RTX OnPresent");
        m_parent->RTX().AdvanceFrameIdForPresentBypass();
      } else {
        m_parent->RTX().OnPresent(immediateContext, m_imageViews.at(imageIndex)->image());

        if (tracePresent)
          D3D11EarlyTrace("D3D11SwapChain::PresentImage after RTX OnPresent");
      }

      // The fallback DX11 path does not run the RTX context frame-end hook,
      // so deferred UI option changes must be flushed here.
      RtxOptionManager::applyPendingValuesOptionLayers();
      RtxOptionManager::applyPendingValues(m_device.ptr());

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage option flush complete");
      
      if (i + 1 >= SyncInterval)
        m_context->signal(m_frameLatencySignal, m_frameId);

      SubmitPresent(immediateContext, sync, i, imageIndex);

      if (tracePresent)
        D3D11EarlyTrace("D3D11SwapChain::PresentImage SubmitPresent complete");
    }

    SyncFrameLatency();

    if (tracePresent)
      D3D11EarlyTrace("D3D11SwapChain::PresentImage complete");

    return S_OK;
  }


  void D3D11SwapChain::SubmitPresent(
          D3D11ImmediateContext*  pContext,
    const vk::PresenterSync&      Sync,
        uint32_t                FrameId,
        uint32_t                ImageIndex) {
    D3D11EarlyTrace("D3D11SwapChain::SubmitPresent enter");

    const bool isD3D11Remix = m_parent->GetOptions()->enableRemix;

    auto lock = pContext->LockContext();

    // Present from CS thread so that we don't
    // have to synchronize with it first.
    m_presentStatus.result = VK_NOT_READY;

    if (isD3D11Remix) {
      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent remix sync path begin");

      Rc<DxvkCommandList> commandList = m_context->endRecording();

      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent remix before submitCommandList");
      m_device->submitCommandList(commandList, Sync.acquire, Sync.present);
      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent remix after submitCommandList");

      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent remix before presentImage");
      m_device->presentImage(0u, false, ImageIndex, m_presenter, &m_presentStatus);
      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent remix after presentImage");
      return;
    }

    pContext->EmitCs([this,
      cIsD3D11Remix = isD3D11Remix,
      cFrameId     = FrameId,
      cImageIndex  = ImageIndex,
      cSync        = Sync,
      cCommandList = m_context->endRecording()
    ] (DxvkContext* ctx) {
      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent CS begin");

      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent before submitCommandList");
      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);
      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent after submitCommandList");

      if (!cIsD3D11Remix
       && m_hud != nullptr
       && !cFrameId) {
        D3D11EarlyTrace("D3D11SwapChain::SubmitPresent before HUD update");
        m_hud->update(1);
        D3D11EarlyTrace("D3D11SwapChain::SubmitPresent after HUD update");
      }

      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent before presentImage");
      m_device->presentImage(0u, false, cImageIndex, m_presenter, &m_presentStatus);
      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent after presentImage");

      D3D11EarlyTrace("D3D11SwapChain::SubmitPresent CS complete");
    });

    pContext->FlushCsChunk();
    D3D11EarlyTrace("D3D11SwapChain::SubmitPresent FlushCsChunk complete");
  }


  void D3D11SwapChain::SynchronizePresent() {
    // Recreate swap chain if the previous present call failed
    VkResult status = m_device->waitForSubmission(&m_presentStatus);
    
    if (status != VK_SUCCESS)
      RecreateSwapChain(m_vsync);
  }


  void D3D11SwapChain::RecreateSwapChain(BOOL Vsync) {
    // Ensure that we can safely destroy the swap chain
    m_device->waitForSubmission(&m_presentStatus);
    m_device->waitForIdle();

    m_presentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)
      throw DxvkError("D3D11SwapChain: Failed to recreate swap chain");
    
    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateFrameLatencyEvent() {
    m_frameLatencySignal = new sync::CallbackFence(m_frameId);

    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
      m_frameLatencyEvent = CreateSemaphore(nullptr, m_frameLatency, DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
  }


  void D3D11SwapChain::CreatePresenter() {
    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();
    presenterDevice.features.fullScreenExclusive = m_device->extensions().extFullScreenExclusive;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = { m_desc.Width, m_desc.Height };
    presenterDesc.imageCount      = PickImageCount(m_desc.BufferCount + 1);
    presenterDesc.numFormats      = PickFormats(m_desc.Format, presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(false, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);
    
    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
  }


  void D3D11SwapChain::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device.ptr(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D11SwapChain::CreateBackBuffer() {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    if (m_backBuffer)
      m_backBuffer->ReleasePrivate();
    
    m_swapImage         = nullptr;
    m_swapImageView     = nullptr;
    m_backBuffer        = nullptr;

    // Create new back buffer
    D3D11_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_desc.Width,  1u);
    desc.Height             = std::max(m_desc.Height, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = m_desc.Format;
    desc.SampleDesc         = m_desc.SampleDesc;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = 0;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = 0;
    desc.TextureLayout      = D3D11_TEXTURE_LAYOUT_UNDEFINED;

    if (m_desc.BufferUsage & DXGI_USAGE_RENDER_TARGET_OUTPUT)
      desc.BindFlags |= D3D11_BIND_RENDER_TARGET;

    if (m_desc.BufferUsage & DXGI_USAGE_SHADER_INPUT)
      desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    if (m_desc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
      desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    
    if (m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE)
      desc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    
    DXGI_USAGE dxgiUsage = DXGI_USAGE_BACK_BUFFER;

    if (m_desc.SwapEffect == DXGI_SWAP_EFFECT_DISCARD
     || m_desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD)
      dxgiUsage |= DXGI_USAGE_DISCARD_ON_PRESENT;

    m_backBuffer = new D3D11Texture2D(m_parent, &desc, dxgiUsage, VK_NULL_HANDLE);
    m_backBuffer->AddRefPrivate();

    m_swapImage = GetCommonTexture(m_backBuffer)->GetImage();

    // Create an image view that allows the
    // image to be bound as a shader resource.
    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type       = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format     = m_swapImage->info().format;
    viewInfo.usage      = VK_IMAGE_USAGE_SAMPLED_BIT;
    viewInfo.aspect     = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel   = 0;
    viewInfo.numLevels  = 1;
    viewInfo.minLayer   = 0;
    viewInfo.numLayers  = 1;
    m_swapImageView = m_device->createImageView(m_swapImage, viewInfo);
    
    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    m_context->clearColorImage(
      m_swapImage, clearColor, subresources);

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D11SwapChain::CreateBlitter() {
    m_blitter = new DxvkSwapchainBlitter(m_device);    
  }


  void D3D11SwapChain::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr)
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
  }


  void D3D11SwapChain::DestroyFrameLatencyEvent() {
    CloseHandle(m_frameLatencyEvent);
  }


  void D3D11SwapChain::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());

    if (m_frameLatencyEvent) {
      m_frameLatencySignal->setCallback(m_frameId, [cFrameLatencyEvent = m_frameLatencyEvent] () {
        ReleaseSemaphore(cFrameLatencyEvent, 1, nullptr);
      });
    }
  }


  uint32_t D3D11SwapChain::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_frameLatency;

    if (!(m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))
      m_dxgiDevice->GetMaximumFrameLatency(&maxFrameLatency);

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_desc.BufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D11SwapChain::PickFormats(
          DXGI_FORMAT               Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D11SwapChain: Unexpected format: ", m_desc.Format));
        
      case DXGI_FORMAT_R8G8B8A8_UNORM:
      case DXGI_FORMAT_B8G8R8A8_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R10G10B10A2_UNORM: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
      
      case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        pDstFormats[n++] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    uint32_t n = 0;

    if (Vsync) {
      if (m_parent->GetOptions()->tearFree == Tristate::False)
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
    } else {
      if (m_parent->GetOptions()->tearFree != Tristate::True)
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
      pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
    }

    return n;
  }


  uint32_t D3D11SwapChain::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    return option > 0 ? uint32_t(option) : uint32_t(Preferred);
  }


  VkFullScreenExclusiveEXT D3D11SwapChain::PickFullscreenMode() {
    return m_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
      ? VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
  }


  std::string D3D11SwapChain::GetApiName() const {
    Com<IDXGIDXVKDevice> device;
    m_parent->QueryInterface(__uuidof(IDXGIDXVKDevice), reinterpret_cast<void**>(&device));

    uint32_t apiVersion = device->GetAPIVersion();
    uint32_t featureLevel = m_parent->GetFeatureLevel();

    uint32_t flHi = (featureLevel >> 12);
    uint32_t flLo = (featureLevel >> 8) & 0x7;

    return str::format("D3D", apiVersion, " FL", flHi, "_", flLo);
  }

}
