#pragma once

#include "d3d11_include.h"

#include <atomic>
#include <mutex>

#include "../dxvk/dxvk_image.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../util/util_threadpool.h"

namespace dxvk {
  class D3D11Buffer;
  class D3D11Device;
  class D3D11DeviceContext;
  class D3D11ImmediateContext;

  struct D3D11Rtx {
    inline static const uint32_t kMaxConcurrentDraws = 2048;
    using GeometryProcessor = WorkerThreadPool<kMaxConcurrentDraws>;

    struct DrawContext {
      bool indexed = false;
      bool indirect = false;
      uint32_t vertexCount = 0;
      uint32_t indexCount = 0;
      uint32_t instanceCount = 0;
      uint32_t firstVertex = 0;
      uint32_t firstIndex = 0;
      int32_t vertexOffset = 0;
      uint32_t firstInstance = 0;
      D3D11Buffer* indirectArgsBuffer = nullptr;
      uint32_t indirectArgsOffset = 0;
    };

    explicit D3D11Rtx(D3D11Device* device);

    bool IsEnabled() const {
      return m_enabled;
    }

    void NotifyDraw(const DrawContext& drawContext);

        void CommitGeometryToRT(
          D3D11DeviceContext*         context,
          const DrawContext&                drawContext);

    void ResetScreenResolution(
            D3D11ImmediateContext*      context,
            uint32_t                    width,
            uint32_t                    height);

    void EndFrame(
            D3D11ImmediateContext*      context,
      const Rc<DxvkImage>&              targetImage,
            bool                        callInjectRtx = true);

    void OnPresent(
            D3D11ImmediateContext*      context,
      const Rc<DxvkImage>&              targetImage);

    void AdvanceFrameIdForPresentBypass();

  private:
    std::mutex m_mutex;
    D3D11Device* const m_parent;
    const std::unique_ptr<GeometryProcessor> m_geometryWorkers;
    bool m_enabled;
    bool m_loggedExperimentalWarning = false;
    std::atomic<bool> m_geometryCaptureDisabled = false;
    uint64_t m_reflexFrameId = 0;
    uint64_t m_pendingDrawCalls = 0;
    uint32_t m_screenWidth = 0;
    uint32_t m_screenHeight = 0;
  };
}