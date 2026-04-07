#pragma once

#include "d3d11_include.h"

#include <atomic>
#include <mutex>

#include "../dxvk/dxvk_cs.h"
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
    ~D3D11Rtx();

    bool IsEnabled() const {
      return m_enabled;
    }

    bool HasRtxExecutionContext() const;

    bool CanUseRtxExecutionContext() const {
      return HasRtxExecutionContext() && !m_auxiliaryBackendFaulted.load(std::memory_order_relaxed);
    }

    bool HasProjectionMatrixThisFrame() const {
      return m_hasProjectionMatrixThisFrame.load(std::memory_order_relaxed);
    }

    bool HasSeenRequiredTransforms() const {
      return m_hasSeenProjectionMatrix.load(std::memory_order_relaxed)
          && m_hasSeenObjectToViewMatrix.load(std::memory_order_relaxed);
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

    template<typename T>
    void EmitRtxCs(T command) {
      if (m_rtxCsThread == nullptr)
        return;

      DxvkCsChunkRef chunk(m_rtxCsChunkPool.allocChunk(DxvkCsChunkFlag::SingleUse), &m_rtxCsChunkPool);

      if (!chunk->push(command)) {
        return;
      }

      m_rtxCsThread->dispatchChunk(std::move(chunk));
    }

    void SynchronizeRtxCs() {
      if (m_rtxCsThread != nullptr)
        m_rtxCsThread->synchronize(DxvkCsThread::SynchronizeAll);
    }

  private:
    bool EnsureRtxExecutionContextLocked();

    std::mutex m_mutex;
    D3D11Device* const m_parent;
    const std::unique_ptr<GeometryProcessor> m_geometryWorkers;
    DxvkCsChunkPool m_rtxCsChunkPool;
    Rc<RtxContext> m_rtxContext;
    std::unique_ptr<DxvkCsThread> m_rtxCsThread;
    bool m_enabled;
    bool m_loggedExperimentalWarning = false;
    bool m_loggedTelemetryOnlyWarning = false;
    bool m_loggedTelemetryCompleteWarning = false;
    bool m_loggedDeferredAuxiliaryContextWarning = false;
    bool m_loggedInferredProjectionWarning = false;
    bool m_loggedInferredObjectToViewWarning = false;
    bool m_loggedMissingProjectionWarning = false;
    bool m_loggedSkippedInjectWarning = false;
    bool m_loggedAuxiliaryPilotModeWarning = false;
    bool m_loggedAuxiliaryPilotFilterWarning = false;
    bool m_loggedAuxiliaryBackendFaultWarning = false;
    std::atomic<bool> m_geometryCaptureFaultedThisFrame = false;
    std::atomic<bool> m_auxiliaryBackendFaulted = false;
    std::atomic<bool> m_hasProjectionMatrixThisFrame = false;
    std::atomic<bool> m_hasSeenProjectionMatrix = false;
    std::atomic<bool> m_hasSeenObjectToViewMatrix = false;
    std::atomic<uint32_t> m_auxiliaryPilotCapturesThisFrame = 0;
    std::atomic<uint32_t> m_geometryCaptureFaultCount = 0;
    uint64_t m_reflexFrameId = 0;
    uint64_t m_pendingDrawCalls = 0;
    uint32_t m_screenWidth = 0;
    uint32_t m_screenHeight = 0;
  };
}