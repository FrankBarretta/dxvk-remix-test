#include "d3d11_rtx.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <glm/gtc/packing.hpp>

#include "d3d11_buffer.h"
#include "d3d11_context.h"
#include "d3d11_context_imm.h"
#include "d3d11_device.h"
#include "d3d11_input_layout.h"
#include "d3d11_rasterizer.h"
#include "d3d11_view_srv.h"
#include "d3d11_trace.h"

#include "../dxvk/rtx_render/rtx_hashing.h"
#include "../dxvk/rtx_render/rtx_context.h"
#include "../dxvk/rtx_render/rtx_matrix_helpers.h"

namespace dxvk {

  namespace {
    constexpr uint32_t kGeometryFaultLogInterval = 256u;

    struct D3D11RtxResolvedAttribute {
      DxvkVertexAttribute attribute;
      const D3D11VertexBufferBinding* binding = nullptr;
    };

    struct D3D11RtxInferredTransforms {
      Matrix4 objectToView;
      Matrix4 viewToProjection;
      bool hasObjectToView = false;
      bool hasViewToProjection = false;
    };

    bool isSupportedTopology(D3D11_PRIMITIVE_TOPOLOGY topology, VkPrimitiveTopology& vkTopology) {
      switch (topology) {
        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
          vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

          return true;

        case D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
          vkTopology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

          return true;

        default:
          return false;
      }
    }

    uint32_t getIndexStride(VkIndexType type) {
      switch (type) {
        case VK_INDEX_TYPE_UINT16:
          return sizeof(uint16_t);

        case VK_INDEX_TYPE_UINT32:
          return sizeof(uint32_t);

        default:
          return 0u;
      }
    }

    bool isPositionFormat(VkFormat format) {
      return format == VK_FORMAT_R32G32B32_SFLOAT
          || format == VK_FORMAT_R32G32B32A32_SFLOAT
          || format == VK_FORMAT_R16G16B16_SFLOAT
          || format == VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    bool isNormalFormat(VkFormat format) {
      return format == VK_FORMAT_R32G32B32_SFLOAT
          || format == VK_FORMAT_R32G32B32A32_SFLOAT
          || format == VK_FORMAT_R16G16B16A16_SFLOAT
          || format == VK_FORMAT_R16G16B16A16_SNORM
          || format == VK_FORMAT_R8G8B8A8_SNORM
          || format == VK_FORMAT_A2B10G10R10_SNORM_PACK32
          || format == VK_FORMAT_R32_UINT;
    }

    bool isTexcoordFormat(VkFormat format) {
      return format == VK_FORMAT_R32G32_SFLOAT
          || format == VK_FORMAT_R32G32B32_SFLOAT
          || format == VK_FORMAT_R32G32B32A32_SFLOAT
          || format == VK_FORMAT_R16G16_SFLOAT
          || format == VK_FORMAT_R16G16_UNORM
          || format == VK_FORMAT_R16G16_SNORM
          || format == VK_FORMAT_R16G16B16A16_SFLOAT;
    }

    bool isColorFormat(VkFormat format) {
      return format == VK_FORMAT_B8G8R8A8_UNORM
          || format == VK_FORMAT_R8G8B8A8_UNORM
          || format == VK_FORMAT_A8B8G8R8_UNORM_PACK32
          || format == VK_FORMAT_R16G16B16A16_UNORM;
    }

    float decodeSnorm16(int16_t value) {
      return std::max(static_cast<float>(value) / 32767.0f, -1.0f);
    }

    bool decodePosition(const uint8_t* vertexData, VkFormat format, Vector3& position) {
      switch (format) {
        case VK_FORMAT_R32G32B32_SFLOAT: {
          const auto* values = reinterpret_cast<const float*>(vertexData);
          position = Vector3(values[0], values[1], values[2]);
          return true;
        }

        case VK_FORMAT_R32G32B32A32_SFLOAT: {
          const auto* values = reinterpret_cast<const float*>(vertexData);
          position = Vector3(values[0], values[1], values[2]);
          return true;
        }

        case VK_FORMAT_R16G16B16_SFLOAT: {
          const auto* values = reinterpret_cast<const uint16_t*>(vertexData);
          position = Vector3(
            glm::unpackHalf1x16(values[0]),
            glm::unpackHalf1x16(values[1]),
            glm::unpackHalf1x16(values[2]));
          return true;
        }

        case VK_FORMAT_R16G16B16A16_SFLOAT: {
          const auto* values = reinterpret_cast<const uint16_t*>(vertexData);
          position = Vector3(
            glm::unpackHalf1x16(values[0]),
            glm::unpackHalf1x16(values[1]),
            glm::unpackHalf1x16(values[2]));
          return true;
        }

        case VK_FORMAT_R16G16B16A16_SNORM: {
          const auto* values = reinterpret_cast<const int16_t*>(vertexData);
          position = Vector3(
            decodeSnorm16(values[0]),
            decodeSnorm16(values[1]),
            decodeSnorm16(values[2]));
          return true;
        }

        default:
          return false;
      }
    }

    bool hasBoundBuffer(const D3D11VertexBufferBinding& binding) {
      return binding.buffer != nullptr && binding.stride != 0u;
    }

    bool isFiniteMatrix(const Matrix4& matrix) {
      for (uint32_t row = 0; row < 4; row++) {
        for (uint32_t col = 0; col < 4; col++) {
          if (!std::isfinite(matrix[row][col]))
            return false;
        }
      }

      return true;
    }

    bool isCandidateAffineTransform(const Matrix4& matrix) {
      if (!isFiniteMatrix(matrix) || isIdentityExact(matrix))
        return false;

      const float epsilon = 1e-3f;

      if (std::abs(matrix[0][3]) > epsilon
       || std::abs(matrix[1][3]) > epsilon
       || std::abs(matrix[2][3]) > epsilon
       || std::abs(matrix[3][3] - 1.0f) > epsilon) {
        return false;
      }

      const float axisLengthX = length(Vector3(matrix[0][0], matrix[0][1], matrix[0][2]));
      const float axisLengthY = length(Vector3(matrix[1][0], matrix[1][1], matrix[1][2]));
      const float axisLengthZ = length(Vector3(matrix[2][0], matrix[2][1], matrix[2][2]));

      const bool validAxisLengths = axisLengthX > epsilon && axisLengthX < 1e4f
                                 && axisLengthY > epsilon && axisLengthY < 1e4f
                                 && axisLengthZ > epsilon && axisLengthZ < 1e4f;

      if (!validAxisLengths)
        return false;

      const Vector3 translation(matrix[3][0], matrix[3][1], matrix[3][2]);
      return length(translation) < 1e7f;
    }

    bool isCandidateProjectionMatrix(const Matrix4& matrix) {
      if (!isFiniteMatrix(matrix) || isIdentityExact(matrix))
        return false;

      DecomposeProjectionParams params;
      decomposeProjection(matrix, params);

      return std::isfinite(params.fov)
          && std::isfinite(params.aspectRatio)
          && std::isfinite(params.nearPlane)
          && std::isfinite(params.farPlane)
          && std::isfinite(params.shearX)
          && std::isfinite(params.shearY)
          && params.fov > 0.2f
          && params.fov < 3.1f
          && std::abs(params.aspectRatio) > 0.3f
          && std::abs(params.aspectRatio) < 5.0f
          && params.nearPlane > 0.0f
          && params.farPlane > params.nearPlane
          && std::abs(params.shearX) < 0.01f
          && std::abs(params.shearY) < 0.01f;
    }

    Matrix4 loadMatrix4(const uint8_t* data) {
      Matrix4 matrix;
      std::memcpy(&matrix, data, sizeof(matrix));
      return matrix;
    }

    bool considerMatrixCandidate(const Matrix4& candidate, D3D11RtxInferredTransforms& inferred) {
      if (!inferred.hasViewToProjection && isCandidateProjectionMatrix(candidate)) {
        inferred.viewToProjection = candidate;
        inferred.hasViewToProjection = true;
      }

      if (!inferred.hasObjectToView && isCandidateAffineTransform(candidate)) {
        inferred.objectToView = candidate;
        inferred.hasObjectToView = true;
      }

      return inferred.hasObjectToView && inferred.hasViewToProjection;
    }

    D3D11RtxInferredTransforms inferTransformsFromVsConstants(const D3D11ContextStateVS& vsState) {
      D3D11RtxInferredTransforms inferred;

      for (const auto& binding : vsState.constantBuffers) {
        if (binding.buffer == nullptr || binding.constantBound < 4u)
          continue;

        const VkDeviceSize byteOffset = static_cast<VkDeviceSize>(binding.constantOffset) * 16u;
        const VkDeviceSize byteLength = static_cast<VkDeviceSize>(binding.constantBound) * 16u;
        const auto slice = binding.buffer->GetBufferSlice(byteOffset, byteLength);
        const auto* data = reinterpret_cast<const uint8_t*>(slice.mapPtr(0));

        if (data == nullptr)
          continue;

        for (VkDeviceSize candidateOffset = 0u; candidateOffset + sizeof(Matrix4) <= byteLength; candidateOffset += 16u) {
          const Matrix4 candidate = loadMatrix4(data + candidateOffset);
          if (considerMatrixCandidate(candidate, inferred))
            return inferred;

          const Matrix4 candidateTranspose = transpose(candidate);
          if (considerMatrixCandidate(candidateTranspose, inferred))
            return inferred;
        }
      }

      return inferred;
    }

    bool resolveIndirectDrawContext(D3D11Rtx::DrawContext& drawContext) {
      if (!drawContext.indirect || drawContext.indirectArgsBuffer == nullptr)
        return !drawContext.indirect;

      const auto argsSlice = drawContext.indirectArgsBuffer->GetBufferSlice(drawContext.indirectArgsOffset);
      const auto* argsData = reinterpret_cast<const uint8_t*>(argsSlice.mapPtr(0));

      if (argsData == nullptr)
        return false;

      if (drawContext.indexed) {
        const auto* args = reinterpret_cast<const VkDrawIndexedIndirectCommand*>(argsData);
        drawContext.vertexCount = 0u;
        drawContext.indexCount = args->indexCount;
        drawContext.instanceCount = args->instanceCount;
        drawContext.firstVertex = 0u;
        drawContext.firstIndex = args->firstIndex;
        drawContext.vertexOffset = args->vertexOffset;
        drawContext.firstInstance = args->firstInstance;
      } else {
        const auto* args = reinterpret_cast<const VkDrawIndirectCommand*>(argsData);
        drawContext.vertexCount = args->vertexCount;
        drawContext.indexCount = 0u;
        drawContext.instanceCount = args->instanceCount;
        drawContext.firstVertex = args->firstVertex;
        drawContext.firstIndex = 0u;
        drawContext.vertexOffset = 0;
        drawContext.firstInstance = args->firstInstance;
      }

      return drawContext.instanceCount != 0u
          && (drawContext.indexed ? drawContext.indexCount != 0u : drawContext.vertexCount != 0u);
    }

    bool resolveAttribute(
      const D3D11InputLayout* inputLayout,
      const D3D11ContextStateIA& iaState,
      bool (*predicate)(VkFormat),
      D3D11RtxResolvedAttribute& result,
      bool preferLowestLocation = true) {
      bool found = false;

      for (const auto& attribute : inputLayout->GetAttributes()) {
        if (!predicate(attribute.format))
          continue;

        if (attribute.binding >= iaState.vertexBuffers.size())
          continue;

        const auto& binding = iaState.vertexBuffers[attribute.binding];

        if (!hasBoundBuffer(binding))
          continue;

        if (!found
         || (preferLowestLocation && attribute.location < result.attribute.location)) {
          result.attribute = attribute;
          result.binding = &binding;
          found = true;
        }
      }

      return found;
    }

    RasterBuffer makeVertexBuffer(const D3D11RtxResolvedAttribute& attribute) {
      return RasterBuffer(
        attribute.binding->buffer->GetBufferSlice(attribute.binding->offset),
        attribute.attribute.offset,
        attribute.binding->stride,
        attribute.attribute.format);
    }

    XXH64_hash_t hashVertexRange(const RasterBuffer& buffer, uint32_t minVertex, uint32_t maxVertex) {
      if (!buffer.defined() || maxVertex < minVertex)
        return kEmptyHash;

      const auto byteOffset = static_cast<size_t>(buffer.offsetFromSlice())
        + static_cast<size_t>(minVertex) * buffer.stride();
      const auto byteSize = static_cast<size_t>(maxVertex - minVertex + 1u) * buffer.stride();
      return hashContiguousMemory(buffer.mapPtr(byteOffset), byteSize);
    }

    template<typename T>
    bool scanIndexRange(const RasterBuffer& indexBuffer, uint32_t indexCount, int32_t baseVertex, uint32_t& minVertex, uint32_t& maxVertex) {
      const auto* indices = reinterpret_cast<const T*>(indexBuffer.mapPtr());
      if (indices == nullptr)
        return false;

      uint32_t localMin = UINT32_MAX;
      uint32_t localMax = 0u;

      for (uint32_t i = 0; i < indexCount; i++) {
        const int64_t adjustedIndex = static_cast<int64_t>(indices[i]) + static_cast<int64_t>(baseVertex);
        if (adjustedIndex < 0)
          return false;

        localMin = std::min(localMin, static_cast<uint32_t>(adjustedIndex));
        localMax = std::max(localMax, static_cast<uint32_t>(adjustedIndex));
      }

      if (localMin == UINT32_MAX)
        return false;

      minVertex = localMin;
      maxVertex = localMax;
      return true;
    }

    RtxContext* getRtxContextOrTrace(DxvkContext* ctx, const char* caller) {
      RtxContext* rtxContext = dynamic_cast<RtxContext*>(ctx);

      if (likely(rtxContext != nullptr))
        return rtxContext;

      D3D11EarlyTrace(caller);
      return nullptr;
    }

  }

  D3D11Rtx::D3D11Rtx(D3D11Device* device)
  : m_parent(device),
    m_geometryWorkers(device->GetOptions()->enableRemix ? std::make_unique<GeometryProcessor>(1, "d3d11-geometry") : nullptr),
    m_enabled(device->GetOptions()->enableRemix) {
  }

  D3D11Rtx::~D3D11Rtx() {
    SynchronizeRtxCs();

  }

  bool D3D11Rtx::HasRtxExecutionContext() const {
    return m_parent->UsesImmediateContextRtx() || m_rtxCsThread != nullptr;
  }

  bool D3D11Rtx::EnsureRtxExecutionContextLocked() {
    if (m_parent->UsesImmediateContextRtx())
      return true;

    if (!m_enabled || !m_parent->GetOptions()->useRtxContext)
      return false;

    if (m_rtxCsThread != nullptr)
      return true;

    m_rtxContext = m_parent->GetDXVKDevice()->createRtxContext();
    m_rtxCsThread = std::make_unique<DxvkCsThread>(m_parent->GetDXVKDevice(), m_rtxContext);

    EmitRtxCs([
      cDevice = m_parent->GetDXVKDevice(),
      cRelaxedBarriers = m_parent->GetOptions()->relaxedBarriers,
      cIgnoreGraphicsBarriers = m_parent->GetOptions()->ignoreGraphicsBarriers
    ] (DxvkContext* ctx) {
      ctx->beginRecording(cDevice->createCommandList());

      DxvkBarrierControlFlags barrierControl;

      if (cRelaxedBarriers)
        barrierControl.set(DxvkBarrierControl::IgnoreWriteAfterWrite);

      if (cIgnoreGraphicsBarriers)
        barrierControl.set(DxvkBarrierControl::IgnoreGraphicsBarriers);

      ctx->setBarrierControl(barrierControl);
    });

    SynchronizeRtxCs();
    Logger::warn("D3D11: Arming the auxiliary RTX command stream now that projection and object-to-view transforms have both been observed.");
    return true;
  }

  namespace {
    const char* getDx11AuxiliaryCommitStageName(uint32_t stage) {
      switch (stage) {
        case 0:
          return "not-entered";
        case 1:
          return "entered";
        case 45:
          return "update-instance-enter";
        case 46:
          return "update-instance-frame-state";
        case 47:
          return "update-instance-first-update-gate";
        case 58:
          return "update-instance-skip-associated-geometry-hash-d3d11-remix";
        case 48:
          return "update-instance-process-buffers";
        case 49:
          return "update-instance-material-hashes";
        case 50:
          return "update-instance-surface-metadata";
        case 51:
          return "update-instance-associated-geometry-hash";
        case 52:
          return "update-instance-transform";
        case 53:
          return "update-instance-instance-flags";
        case 54:
          return "update-instance-mask";
        case 55:
          return "update-instance-billboards";
        case 56:
          return "update-instance-callbacks";
        case 57:
          return "update-instance-complete";
        case 101:
          return "finalize-pending-futures-entered";
        case 102:
          return "geometry-hashes-finalized";
        case 103:
          return "bounding-box-finalized";
        case 104:
          return "skinning-finalized";
        case 105:
          return "geometry-categories-finalized";
        case 106:
          return "geometry-hashes-invalid";
        case 111:
          return "geometry-hashes-check-valid";
        case 112:
          return "geometry-hashes-get";
        case 113:
          return "geometry-hashes-validated";
        case 114:
          return "geometry-hashes-complete";
        case 121:
          return "bounding-box-check-valid";
        case 122:
          return "bounding-box-get";
        case 123:
          return "bounding-box-complete";
        case 131:
          return "skinning-check-valid";
        case 132:
          return "skinning-get";
        case 133:
          return "skinning-validated";
        case 134:
          return "skinning-camera-transform";
        case 135:
          return "skinning-no-camera";
        case 136:
          return "skinning-single-bone-fold";
        case 137:
          return "skinning-complete";
        case 141:
          return "geometry-category-rule-copy";
        case 147:
          return "geometry-category-hash";
        case 148:
          return "geometry-category-skipped-d3d11-remix";
        case 142:
          return "geometry-category-lookup";
        case 143:
          return "geometry-category-complete";
        case 144:
          return "geometry-asset-hash-geometry-component";
        case 160:
          return "geometry-asset-hash-positions";
        case 170:
          return "geometry-asset-hash-rule-bits-copy";
        case 171:
          return "geometry-asset-hash-positions-rule-passed";
        case 172:
          return "geometry-asset-hash-positions-read";
        case 173:
          return "geometry-asset-hash-positions-read-complete";
        case 174:
          return "geometry-asset-hash-positions-combined";
        case 161:
          return "geometry-asset-hash-legacy-positions-0";
        case 162:
          return "geometry-asset-hash-legacy-positions-1";
        case 163:
          return "geometry-asset-hash-texcoords";
        case 164:
          return "geometry-asset-hash-indices";
        case 165:
          return "geometry-asset-hash-legacy-indices";
        case 166:
          return "geometry-asset-hash-geometry-descriptor";
        case 167:
          return "geometry-asset-hash-vertex-layout";
        case 168:
          return "geometry-asset-hash-vertex-shader";
        case 169:
          return "geometry-asset-hash-geometry-complete";
        case 145:
          return "geometry-asset-hash-material-component";
        case 146:
          return "geometry-asset-hash-combined";
        case 2:
          return "futures-finalized";
        case 3:
          return "camera-processed";
        case 4:
          return "unknown-camera-skipped";
        case 5:
          return "sky-skip-submit";
        case 6:
          return "sky-handled";
        case 7:
          return "terrain-baked";
        case 8:
          return "precombined-matrices-resolved";
        case 9:
          return "ready-to-submit-draw-state";
        case 10:
          return "submit-draw-state-complete";
        case 11:
          return "finalize-pending-futures-returned-false";
        case 12:
          return "submit-draw-state-enter";
        case 13:
          return "submit-draw-state-fog-processed";
        case 14:
          return "submit-draw-state-active-replacement-hash";
        case 15:
          return "submit-draw-state-track-mesh-hash";
        case 16:
          return "submit-draw-state-replacement-lookup";
        case 17:
          return "submit-draw-state-material-determined";
        case 18:
          return "submit-draw-state-draw-replacements";
        case 19:
          return "submit-draw-state-process-draw-call";
        case 20:
          return "submit-draw-state-exit";
        case 21:
          return "submit-draw-state-skip-replacement-lookup-d3d11-remix";
        case 30:
          return "process-draw-call-enter";
        case 31:
          return "process-draw-call-cache-lookup";
        case 32:
          return "process-draw-call-cache-hit";
        case 33:
          return "process-draw-call-cache-miss";
        case 34:
          return "process-draw-call-cache-result-ready";
        case 35:
          return "process-draw-call-dispatch-skinning";
        case 36:
          return "process-draw-call-process-scene-object";
        case 37:
          return "process-draw-call-create-effect-light";
        case 38:
          return "process-draw-call-object-picking-meta";
        case 39:
          return "process-draw-call-object-picking-lock";
        case 40:
          return "process-scene-object-enter";
        case 41:
          return "process-scene-object-find-similar-instance";
        case 42:
          return "process-scene-object-add-instance";
        case 43:
          return "process-scene-object-update-instance";
        case 44:
          return "process-scene-object-complete";
        default:
          return "unknown-stage";
      }
    }

    bool TryCommitGeometryToRt(RtxContext* rtxContext, const DrawParameters& params, DrawCallState& drawCallState) {
#ifdef _MSC_VER
      __try {
        rtxContext->commitGeometryToRT(params, drawCallState);
        return true;
      } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
#else
      rtxContext->commitGeometryToRT(params, drawCallState);
      return true;
#endif
    }

    bool TryResetScreenResolution(RtxContext* rtxContext, uint32_t width, uint32_t height) {
#ifdef _MSC_VER
      __try {
        rtxContext->resetScreenResolution({ width, height, 1u });
        return true;
      } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
      }
#else
      rtxContext->resetScreenResolution({ width, height, 1u });
      return true;
#endif
    }

  }


  void D3D11Rtx::NotifyDraw(const DrawContext& drawContext) {
    if (!m_enabled)
      return;

    if (!CanUseRtxExecutionContext()
     && m_hasSeenProjectionMatrix.load(std::memory_order_relaxed)
     && m_hasSeenObjectToViewMatrix.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_pendingDrawCalls += 1;

    if (CanUseRtxExecutionContext() && !m_loggedExperimentalWarning) {
      m_loggedExperimentalWarning = true;
      if (!m_parent->UsesImmediateContextRtx()) {
        Logger::warn(str::format(
          "D3D11: Experimental auxiliary Remix pilot enabled. DX11 currently captures up to ",
          std::max(1, m_parent->GetOptions()->remixPilotMaxCapturesPerFrame),
          " indexed triangle-list draws per frame while frame hooks remain disabled."));
      } else {
        Logger::warn("D3D11: Experimental Remix path enabled. D3D11 draws feed an auxiliary RTX command stream for supported triangle captures.");
      }
    }

    if (!CanUseRtxExecutionContext() && !m_loggedTelemetryOnlyWarning) {
      m_loggedTelemetryOnlyWarning = true;
      Logger::info("D3D11: Running stable DXVK context while collecting Remix transform telemetry from D3D11 draws.");
    }

    if (!CanUseRtxExecutionContext()
     && m_parent->GetOptions()->useRtxContext
     && !m_loggedDeferredAuxiliaryContextWarning) {
      m_loggedDeferredAuxiliaryContextWarning = true;
      Logger::warn("D3D11: Deferring auxiliary RTX command stream creation until the required transforms have been inferred at least once.");
    }

    (void)drawContext;
  }


  void D3D11Rtx::CommitGeometryToRT(
          D3D11DeviceContext*         context,
    const DrawContext&                drawContext) {
    if (!m_enabled || m_geometryCaptureFaultedThisFrame.load(std::memory_order_relaxed))
      return;

    const bool usingAuxiliaryPilot = !m_parent->UsesImmediateContextRtx();
    const bool usingAuxiliaryInjectRtxProbe = usingAuxiliaryPilot
      && m_parent->GetOptions()->remixPilotEnableFullEndFrame
      && m_parent->GetOptions()->remixPilotEnableInjectRtx
      && std::max(0, m_parent->GetOptions()->remixPilotInjectRtxStageLimit) > 0;
    const bool auxiliaryInjectRtxProbeCompleted = m_auxiliaryInjectRtxProbeCompleted.load(std::memory_order_relaxed);
    const uint32_t successfulPilotCaptures = m_auxiliaryPilotSuccessfulCaptures.load(std::memory_order_relaxed);
    const uint32_t maxSuccessfulPilotCaptures = static_cast<uint32_t>(std::max(0, m_parent->GetOptions()->remixPilotMaxSuccessfulCaptures));

    if (usingAuxiliaryPilot
     && maxSuccessfulPilotCaptures > 0u
     && successfulPilotCaptures >= maxSuccessfulPilotCaptures) {
      if (!m_loggedAuxiliaryPilotCompletedWarning) {
        m_loggedAuxiliaryPilotCompletedWarning = true;
        Logger::warn("D3D11: Auxiliary Remix pilot reached its configured successful-capture limit; further DX11 geometry capture is disabled to preserve performance.");
      }

      return;
    }

    if (usingAuxiliaryPilot && successfulPilotCaptures > 0u) {
      const uint64_t configuredPilotCaptureFrameInterval = static_cast<uint64_t>(std::max(0, m_parent->GetOptions()->remixPilotCaptureInterval));
      const uint64_t configuredPostProbeCaptureFrameInterval = static_cast<uint64_t>(std::max(0, m_parent->GetOptions()->remixPilotPostProbeCaptureInterval));
      const uint64_t pilotCaptureFrameInterval = auxiliaryInjectRtxProbeCompleted
        ? std::max(configuredPilotCaptureFrameInterval, configuredPostProbeCaptureFrameInterval)
        : configuredPilotCaptureFrameInterval;

      if (pilotCaptureFrameInterval > 0u
       && m_lastAuxiliaryPilotCaptureFrame != UINT64_MAX
       && m_reflexFrameId < m_lastAuxiliaryPilotCaptureFrame + pilotCaptureFrameInterval) {
        if (!m_loggedAuxiliaryPilotThrottleWarning) {
          m_loggedAuxiliaryPilotThrottleWarning = true;
          Logger::warn(str::format(
            "D3D11: Throttling the auxiliary Remix pilot to one supported draw every ",
            pilotCaptureFrameInterval,
            " frames while frame hooks remain disabled to avoid severe frame-time collapse."));
        }

        return;
      }
    }

    if (!CanUseRtxExecutionContext()
     && m_hasSeenProjectionMatrix.load(std::memory_order_relaxed)
     && m_hasSeenObjectToViewMatrix.load(std::memory_order_relaxed)) {
      return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_parent->UsesImmediateContextRtx()
     && m_auxiliaryPilotResetPending.exchange(false, std::memory_order_relaxed)) {
      SynchronizeRtxCs();
      m_rtxCsThread = nullptr;
      m_rtxContext = nullptr;

      if (!m_loggedAuxiliaryPilotResetWarning) {
        m_loggedAuxiliaryPilotResetWarning = true;
        Logger::warn("D3D11: Resetting the auxiliary RTX context between successful pilot captures to avoid persistent frame-time collapse.");
      }
    }

    const auto inferredTransforms = inferTransformsFromVsConstants(context->m_state.vs);

    if (inferredTransforms.hasViewToProjection && !m_loggedInferredProjectionWarning) {
      m_loggedInferredProjectionWarning = true;
      Logger::info("D3D11: Inferred a projection matrix from VS constant buffers for Remix camera detection.");
    }

    if (inferredTransforms.hasViewToProjection) {
      m_hasProjectionMatrixThisFrame.store(true, std::memory_order_relaxed);
      m_hasSeenProjectionMatrix.store(true, std::memory_order_relaxed);
    }

    if (inferredTransforms.hasObjectToView && !m_loggedInferredObjectToViewWarning) {
      m_loggedInferredObjectToViewWarning = true;
      Logger::info("D3D11: Inferred an object-to-view matrix from VS constant buffers for Remix geometry placement.");
    }

    if (inferredTransforms.hasObjectToView) {
      m_hasSeenObjectToViewMatrix.store(true, std::memory_order_relaxed);
    }

    if (!CanUseRtxExecutionContext()
     && m_hasSeenProjectionMatrix.load(std::memory_order_relaxed)
     && m_hasSeenObjectToViewMatrix.load(std::memory_order_relaxed)
     && !m_loggedTelemetryCompleteWarning) {
      m_loggedTelemetryCompleteWarning = true;
      Logger::info("D3D11: Remix transform telemetry is complete; stopping further constant-buffer scans on the stable path.");
    }

    if (!CanUseRtxExecutionContext()
     && m_hasSeenProjectionMatrix.load(std::memory_order_relaxed)
     && m_hasSeenObjectToViewMatrix.load(std::memory_order_relaxed)) {
      if (!EnsureRtxExecutionContextLocked())
        return;

      if (!m_parent->UsesImmediateContextRtx()) {
        if (!m_loggedAuxiliaryPilotModeWarning) {
          m_loggedAuxiliaryPilotModeWarning = true;
          Logger::warn(str::format(
            "D3D11: Experimental auxiliary Remix pilot enabled. DX11 currently captures up to ",
            std::max(1, m_parent->GetOptions()->remixPilotMaxCapturesPerFrame),
            " indexed triangle-list draws per frame while frame hooks remain disabled."));
        }
      } else if (!m_loggedExperimentalWarning) {
        m_loggedExperimentalWarning = true;
        Logger::warn("D3D11: Experimental Remix path enabled. D3D11 draws feed an auxiliary RTX command stream for supported triangle captures.");
      }
    }

    if (!CanUseRtxExecutionContext()) {
      return;
    }

    DrawContext resolvedDrawContext = drawContext;
    if (!resolveIndirectDrawContext(resolvedDrawContext))
      return;

    auto& iaState = context->m_state.ia;

    if (iaState.inputLayout == nullptr)
      return;

    VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    if (!isSupportedTopology(iaState.primitiveTopology, vkTopology))
      return;

    if (!m_parent->UsesImmediateContextRtx()) {
      const uint64_t configuredPilotCaptureFrameInterval = static_cast<uint64_t>(std::max(0, m_parent->GetOptions()->remixPilotCaptureInterval));
      const uint64_t configuredProbeCaptureFrameInterval = static_cast<uint64_t>(std::max(0, m_parent->GetOptions()->remixPilotProbeCaptureInterval));
      const uint64_t configuredPostProbeCaptureFrameInterval = static_cast<uint64_t>(std::max(0, m_parent->GetOptions()->remixPilotPostProbeCaptureInterval));
      const uint32_t configuredMaxPilotCapturesPerFrame = static_cast<uint32_t>(std::max(1, m_parent->GetOptions()->remixPilotMaxCapturesPerFrame));
      const uint32_t configuredProbeMaxPilotCapturesPerFrame = static_cast<uint32_t>(std::max(1, m_parent->GetOptions()->remixPilotProbeMaxCapturesPerFrame));
      const uint32_t configuredPostProbeMaxPilotCapturesPerFrame = static_cast<uint32_t>(std::max(1, m_parent->GetOptions()->remixPilotPostProbeMaxCapturesPerFrame));
      const uint64_t pilotCaptureFrameInterval = usingAuxiliaryInjectRtxProbe
        ? std::max(configuredPilotCaptureFrameInterval, configuredProbeCaptureFrameInterval)
        : auxiliaryInjectRtxProbeCompleted
          ? std::max(configuredPilotCaptureFrameInterval, configuredPostProbeCaptureFrameInterval)
          : configuredPilotCaptureFrameInterval;
      const uint32_t maxPilotCapturesPerFrame = usingAuxiliaryInjectRtxProbe
        ? std::min(configuredMaxPilotCapturesPerFrame, configuredProbeMaxPilotCapturesPerFrame)
        : auxiliaryInjectRtxProbeCompleted
          ? std::min(configuredMaxPilotCapturesPerFrame, configuredPostProbeMaxPilotCapturesPerFrame)
          : configuredMaxPilotCapturesPerFrame;
      const bool supportedPilotDraw = resolvedDrawContext.indexed
        && resolvedDrawContext.instanceCount <= 1u
        && vkTopology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

      if (!supportedPilotDraw) {
        if (!m_loggedAuxiliaryPilotFilterWarning) {
          m_loggedAuxiliaryPilotFilterWarning = true;
          Logger::warn("D3D11: Auxiliary Remix pilot is skipping non-indexed, instanced, or non-triangle-list draws while geometry capture is reintroduced incrementally.");
        }

        return;
      }

      if (m_auxiliaryPilotCapturesThisFrame.load(std::memory_order_relaxed) >= maxPilotCapturesPerFrame)
        return;

      if (pilotCaptureFrameInterval > 0u
       && m_lastAuxiliaryPilotCaptureFrame != UINT64_MAX
       && m_reflexFrameId < m_lastAuxiliaryPilotCaptureFrame + pilotCaptureFrameInterval) {
        if (!m_loggedAuxiliaryPilotThrottleWarning) {
          m_loggedAuxiliaryPilotThrottleWarning = true;
          Logger::warn(str::format(
            "D3D11: Throttling the auxiliary Remix pilot to up to ",
            maxPilotCapturesPerFrame,
            " supported draws every ",
            pilotCaptureFrameInterval,
            usingAuxiliaryInjectRtxProbe
              ? " frames while the auxiliary injectRTX probe is active to avoid severe frame-time collapse."
              : auxiliaryInjectRtxProbeCompleted
                ? " frames after the successful auxiliary injectRTX probe to keep the DX11 capture-only baseline playable."
                : " frames while frame hooks remain disabled to avoid severe frame-time collapse."));
        }

        return;
      }
    }

    VkIndexType indexType = VK_INDEX_TYPE_NONE_KHR;
    if (resolvedDrawContext.indexed) {
      if (iaState.indexBuffer.buffer == nullptr || resolvedDrawContext.indexCount == 0u)
        return;

      switch (iaState.indexBuffer.format) {
        case DXGI_FORMAT_R16_UINT:
          indexType = VK_INDEX_TYPE_UINT16;
          break;

        case DXGI_FORMAT_R32_UINT:
          indexType = VK_INDEX_TYPE_UINT32;
          break;

        default:
          return;
      }
    }

    D3D11RtxResolvedAttribute positionAttribute;
    if (!resolveAttribute(iaState.inputLayout.ptr(), iaState, isPositionFormat, positionAttribute))
      return;

    D3D11RtxResolvedAttribute normalAttribute;
    D3D11RtxResolvedAttribute texcoordAttribute;
    D3D11RtxResolvedAttribute colorAttribute;

    resolveAttribute(iaState.inputLayout.ptr(), iaState, isNormalFormat, normalAttribute, false);
    resolveAttribute(iaState.inputLayout.ptr(), iaState, isTexcoordFormat, texcoordAttribute, false);
    resolveAttribute(iaState.inputLayout.ptr(), iaState, isColorFormat, colorAttribute, false);

    if (resolvedDrawContext.vertexOffset < 0)
      return;

    if (normalAttribute.binding != nullptr && normalAttribute.attribute.location == positionAttribute.attribute.location)
      normalAttribute.binding = nullptr;

    if (texcoordAttribute.binding != nullptr && texcoordAttribute.attribute.location == positionAttribute.attribute.location)
      texcoordAttribute.binding = nullptr;

    if (colorAttribute.binding != nullptr && colorAttribute.attribute.location == positionAttribute.attribute.location)
      colorAttribute.binding = nullptr;

    RasterGeometry geometryData;
    geometryData.topology = vkTopology;
    geometryData.positionBuffer = makeVertexBuffer(positionAttribute);
    if (resolvedDrawContext.indexed) {
      geometryData.indexBuffer = RasterBuffer(
        iaState.indexBuffer.buffer->GetBufferSlice(
          iaState.indexBuffer.offset + static_cast<VkDeviceSize>(resolvedDrawContext.firstIndex) * getIndexStride(indexType)),
        0u,
        getIndexStride(indexType),
        indexType);
      geometryData.indexCount = resolvedDrawContext.indexCount;
    }

    if (normalAttribute.binding != nullptr)
      geometryData.normalBuffer = makeVertexBuffer(normalAttribute);

    if (texcoordAttribute.binding != nullptr)
      geometryData.texcoordBuffer = makeVertexBuffer(texcoordAttribute);

    if (colorAttribute.binding != nullptr)
      geometryData.color0Buffer = makeVertexBuffer(colorAttribute);

    uint32_t minVertex = resolvedDrawContext.firstVertex;
    uint32_t maxVertex = resolvedDrawContext.firstVertex;
    bool haveValidRange = resolvedDrawContext.vertexCount != 0u;

    if (resolvedDrawContext.indexed) {
      haveValidRange = false;

      switch (indexType) {
        case VK_INDEX_TYPE_UINT16:
          haveValidRange = scanIndexRange<uint16_t>(geometryData.indexBuffer, resolvedDrawContext.indexCount, resolvedDrawContext.vertexOffset, minVertex, maxVertex);
          break;

        case VK_INDEX_TYPE_UINT32:
          haveValidRange = scanIndexRange<uint32_t>(geometryData.indexBuffer, resolvedDrawContext.indexCount, resolvedDrawContext.vertexOffset, minVertex, maxVertex);
          break;

        default:
          break;
      }
    } else {
      maxVertex = resolvedDrawContext.firstVertex + resolvedDrawContext.vertexCount - 1u;
    }

    if (!haveValidRange)
      return;

    geometryData.vertexCount = resolvedDrawContext.indexed ? maxVertex + 1u : resolvedDrawContext.vertexCount;

    if (const auto* rasterizerState = context->m_state.rs.state) {
      const auto* desc = rasterizerState->Desc();
      switch (desc->CullMode) {
        case D3D11_CULL_FRONT:
          geometryData.cullMode = VK_CULL_MODE_FRONT_BIT;
          break;

        case D3D11_CULL_BACK:
          geometryData.cullMode = VK_CULL_MODE_BACK_BIT;
          break;

        default:
          geometryData.cullMode = VK_CULL_MODE_NONE;
          break;
      }

      geometryData.frontFace = desc->FrontCounterClockwise
        ? VK_FRONT_FACE_COUNTER_CLOCKWISE
        : VK_FRONT_FACE_CLOCKWISE;
    }

    GeometryHashes hashes;
    hashes[HashComponents::GeometryDescriptor] = hashGeometryDescriptor(
      geometryData.indexCount,
      geometryData.vertexCount,
      resolvedDrawContext.indexed ? geometryData.indexBuffer.indexType() : VK_INDEX_TYPE_NONE_KHR,
      geometryData.topology);
    hashes[HashComponents::VertexLayout] = hashVertexLayout(geometryData);
    hashes[HashComponents::VertexPosition] = hashVertexRange(geometryData.positionBuffer, minVertex, maxVertex);

    if (geometryData.texcoordBuffer.defined())
      hashes[HashComponents::VertexTexcoord] = hashVertexRange(geometryData.texcoordBuffer, minVertex, maxVertex);

    if (resolvedDrawContext.indexed) {
      hashes[HashComponents::Indices] = hashContiguousMemory(
        geometryData.indexBuffer.mapPtr(),
        static_cast<size_t>(geometryData.indexCount) * geometryData.indexBuffer.stride());
    }
    hashes.precombine();

    if (m_geometryWorkers == nullptr)
      return;

    geometryData.futureGeometryHashes = m_geometryWorkers->Schedule([hashes]() mutable {
      return hashes;
    });

    geometryData.futureBoundingBox = m_geometryWorkers->Schedule([positionBuffer = geometryData.positionBuffer, minVertex, maxVertex]() {
      const uint8_t* vertexData = reinterpret_cast<const uint8_t*>(
        positionBuffer.mapPtr(static_cast<VkDeviceSize>(positionBuffer.offsetFromSlice()) + static_cast<VkDeviceSize>(minVertex) * positionBuffer.stride()));

      __m128 minPos = _mm_set_ps1(FLT_MAX);
      __m128 maxPos = _mm_set_ps1(-FLT_MAX);

      for (uint32_t vertexIdx = minVertex; vertexIdx <= maxVertex; vertexIdx++) {
        Vector3 position;
        if (!decodePosition(vertexData, positionBuffer.vertexFormat(), position)) {
          return AxisAlignedBoundingBox {
            Vector3(),
            Vector3()
          };
        }

        const __m128 value = _mm_set_ps(0.0f, position.z, position.y, position.x);
        minPos = _mm_min_ps(minPos, value);
        maxPos = _mm_max_ps(maxPos, value);
        vertexData += positionBuffer.stride();
      }

      return AxisAlignedBoundingBox {
        Vector3 { minPos.m128_f32[0], minPos.m128_f32[1], minPos.m128_f32[2] },
        Vector3 { maxPos.m128_f32[0], maxPos.m128_f32[1], maxPos.m128_f32[2] }
      };
    });

    DrawCallState drawCallState;
    drawCallState.geometryData = std::move(geometryData);
    drawCallState.drawCallID = static_cast<uint32_t>(m_pendingDrawCalls);
    drawCallState.usesVertexShader = context->m_state.vs.shader != nullptr;
    drawCallState.usesPixelShader = context->m_state.ps.shader != nullptr;
    if (context->m_state.rs.numViewports != 0u) {
      const auto& viewport = context->m_state.rs.viewports[0];
      drawCallState.minZ = std::clamp(viewport.MinDepth, 0.0f, 1.0f);
      drawCallState.maxZ = std::clamp(viewport.MaxDepth, 0.0f, 1.0f);
    }
    if (context->m_state.om.dsState != nullptr) {
      D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
      context->m_state.om.dsState->GetDesc(&depthStencilDesc);
      drawCallState.zEnable = depthStencilDesc.DepthEnable;
      drawCallState.zWriteEnable = depthStencilDesc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
      drawCallState.stencilEnabled = depthStencilDesc.StencilEnable;
    }
    drawCallState.transformData.objectToWorld = Matrix4();
    drawCallState.transformData.worldToView = inferredTransforms.hasObjectToView
      ? inferredTransforms.objectToView
      : Matrix4();
    drawCallState.transformData.objectToView = inferredTransforms.hasObjectToView
      ? inferredTransforms.objectToView
      : Matrix4();
    drawCallState.transformData.viewToProjection = inferredTransforms.hasViewToProjection
      ? inferredTransforms.viewToProjection
      : Matrix4();

    if (!inferredTransforms.hasViewToProjection) {
      if (!m_loggedMissingProjectionWarning) {
        m_loggedMissingProjectionWarning = true;
        Logger::warn("D3D11: Skipping Remix geometry capture until a projection matrix can be inferred from VS constant buffers.");
      }

      return;
    }

    for (const auto& shaderResourceView : context->m_state.ps.shaderResources.views) {
      if (shaderResourceView == nullptr)
        continue;

      const Rc<DxvkImageView> colorImageView = shaderResourceView->GetImageView();
      if (colorImageView == nullptr)
        continue;

      drawCallState.materialData = LegacyMaterialData(TextureRef(colorImageView), TextureRef(), D3DMATERIAL9 {});
      break;
    }

    drawCallState.setupCategoriesForTexture();

    DrawParameters params;
    params.vertexCount = resolvedDrawContext.vertexCount;
    params.indexCount = resolvedDrawContext.indexCount;
    params.instanceCount = resolvedDrawContext.instanceCount;
    params.firstIndex = 0u;
    params.vertexOffset = resolvedDrawContext.indexed
      ? static_cast<uint32_t>(resolvedDrawContext.vertexOffset)
      : resolvedDrawContext.firstVertex;

    if (!m_parent->UsesImmediateContextRtx())
      m_auxiliaryPilotCapturesThisFrame.fetch_add(1u, std::memory_order_relaxed);

    if (!m_parent->UsesImmediateContextRtx())
      m_lastAuxiliaryPilotCaptureFrame = m_reflexFrameId;

    auto emitCommit = [this, params, usingAuxiliaryInjectRtxProbe, drawCallState = std::move(drawCallState)](DxvkContext* ctx) mutable {
      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::CommitGeometryToRT skipped because the command stream is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      if (TryCommitGeometryToRt(rtxContext, params, drawCallState)) {
        if (!m_parent->UsesImmediateContextRtx()) {
          m_auxiliaryPilotSuccessfulCaptures.fetch_add(1u, std::memory_order_relaxed);

          if (!usingAuxiliaryInjectRtxProbe && !m_auxiliaryInjectRtxProbeCompleted.load(std::memory_order_relaxed)) {
            m_auxiliaryPilotResetPending.store(true, std::memory_order_relaxed);
          }
        }

        m_geometryCaptureFaultCount.store(0u, std::memory_order_relaxed);
        return;
      }

      m_auxiliaryBackendFaulted.store(true, std::memory_order_relaxed);
      const uint32_t faultCount = m_geometryCaptureFaultCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
      m_geometryCaptureFaultedThisFrame.store(true, std::memory_order_relaxed);

      if (!m_loggedAuxiliaryBackendFaultWarning) {
        m_loggedAuxiliaryBackendFaultWarning = true;
        Logger::warn("D3D11: Disabling the auxiliary RTX command stream after a backend fault. Falling back to telemetry-only DX11 Remix behavior.");
      }

      Logger::warn(str::format(
        "D3D11: Auxiliary commitGeometryToRT faulted for drawCallID ", drawCallState.drawCallID,
        " after stage ", drawCallState.remixDebugCommitStage,
        " (", getDx11AuxiliaryCommitStageName(drawCallState.remixDebugCommitStage), ")."));

      if (faultCount <= 4u) {
        Logger::warn(str::format("D3D11: Suppressing further Remix geometry capture until the next frame after backend fault ", faultCount, "."));
      }

      if (faultCount % kGeometryFaultLogInterval == 0u) {
        Logger::warn(str::format("D3D11: commitGeometryToRT has seen ", faultCount, " backend faults so far; per-frame suppression remains active."));
      }
    };

    if (m_parent->UsesImmediateContextRtx()) {
      context->EmitCs(std::move(emitCommit));
    } else {
      EmitRtxCs(std::move(emitCommit));
    }
  }


  void D3D11Rtx::ResetScreenResolution(
          D3D11ImmediateContext*      context,
          uint32_t                    width,
          uint32_t                    height) {
    if (!m_enabled || !CanUseRtxExecutionContext())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    D3D10DeviceLock contextLock = context->LockContext();

    if (!m_parent->UsesImmediateContextRtx() && !m_loggedAuxiliaryResetScreenResolutionWarning) {
      m_loggedAuxiliaryResetScreenResolutionWarning = true;
      Logger::warn("D3D11: Auxiliary Remix pilot is running ResetScreenResolution before scene capture end frame.");
    }

    if (m_screenWidth == width && m_screenHeight == height)
      return;

    m_screenWidth = width;
    m_screenHeight = height;

    auto emitReset = [cWidth = width, cHeight = height](DxvkContext* ctx) {
      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::ResetScreenResolution skipped because the command stream is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      TryResetScreenResolution(rtxContext, cWidth, cHeight);
    };

    if (m_parent->UsesImmediateContextRtx())
      context->EmitCs(std::move(emitReset));
    else
      EmitRtxCs(std::move(emitReset));
  }


  void D3D11Rtx::EndFrame(
          D3D11ImmediateContext*      context,
    const Rc<DxvkImage>&              targetImage,
          bool                        callInjectRtx) {
    if (!m_enabled || !CanUseRtxExecutionContext())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    D3D10DeviceLock contextLock = context->LockContext();

    const bool traceFrame = m_reflexFrameId < 8;
    const bool useAuxiliarySceneCaptureOnly = !m_parent->UsesImmediateContextRtx();
    const bool auxiliaryFullEndFrameRequested = useAuxiliarySceneCaptureOnly
      && m_parent->GetOptions()->remixPilotEnableFullEndFrame;
    const bool auxiliaryFullEndFrameAfterProbeRequested = useAuxiliarySceneCaptureOnly
      && m_parent->GetOptions()->remixPilotEnableFullEndFrameAfterProbe;
    const uint32_t auxiliaryInjectRtxStageLimit = static_cast<uint32_t>(std::max(0, m_parent->GetOptions()->remixPilotInjectRtxStageLimit));
    const bool useAuxiliaryInjectRtxRequested = auxiliaryFullEndFrameRequested
      && m_parent->GetOptions()->remixPilotEnableInjectRtx;
    const bool auxiliaryInjectRtxProbeAlreadyCompleted = m_auxiliaryInjectRtxProbeCompleted.load(std::memory_order_relaxed);
    const bool useAuxiliaryInjectRtxProbe = useAuxiliaryInjectRtxRequested
      && !auxiliaryInjectRtxProbeAlreadyCompleted
      && auxiliaryInjectRtxStageLimit > 0u;
    const bool useAuxiliaryFullEndFrame = auxiliaryFullEndFrameRequested
      && !auxiliaryInjectRtxProbeAlreadyCompleted;
    const bool useAuxiliaryFullEndFrameAfterProbe = auxiliaryFullEndFrameAfterProbeRequested
      && auxiliaryInjectRtxProbeAlreadyCompleted;
    const bool useAuxiliaryInjectRtxAfterProbe = useAuxiliaryFullEndFrameAfterProbe
      && m_parent->GetOptions()->remixPilotEnableInjectRtxAfterProbe;
    const bool shouldRunAuxiliaryInjectRtxAfterProbe = useAuxiliaryInjectRtxAfterProbe
      && m_lastAuxiliaryPilotCaptureFrame != UINT64_MAX
      && m_lastAuxiliaryPilotCaptureFrame != m_lastAuxiliaryInjectRtxCaptureFrame;
    const bool useAuxiliaryAnyFullEndFrame = useAuxiliaryFullEndFrame
      || useAuxiliaryFullEndFrameAfterProbe;

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::EndFrame enter");

    if (useAuxiliarySceneCaptureOnly) {
      auto logAuxiliaryInjectProbeStep = [useAuxiliaryInjectRtxProbe](const char* step) {
        if (useAuxiliaryInjectRtxProbe) {
          Logger::warn(str::format("D3D11: Auxiliary injectRTX probe ", step, "."));
        }
      };

      logAuxiliaryInjectProbeStep("before Flush");

      context->Flush();

      logAuxiliaryInjectProbeStep("after Flush");
      logAuxiliaryInjectProbeStep("before pre-Emit SynchronizeRtxCs");

      SynchronizeRtxCs();

      logAuxiliaryInjectProbeStep("after pre-Emit SynchronizeRtxCs");

      if (useAuxiliaryAnyFullEndFrame) {
        if (!useAuxiliaryInjectRtxProbe && !m_loggedAuxiliaryFullEndFrameWarning) {
          m_loggedAuxiliaryFullEndFrameWarning = true;
          Logger::warn("D3D11: Auxiliary Remix pilot is running full endFrame with injectRTX still disabled after successful geometry capture.");
        }

        if (useAuxiliaryFullEndFrameAfterProbe && !m_loggedAuxiliaryFullEndFrameAfterProbeWarning) {
          m_loggedAuxiliaryFullEndFrameAfterProbeWarning = true;
          Logger::warn("D3D11: Auxiliary Remix pilot is keeping full endFrame active after the successful injectRTX probe so visible Remix output can be evaluated on DX11.");
        }

        if (useAuxiliaryInjectRtxProbe && !m_loggedAuxiliaryInjectRtxProbeWarning) {
          m_loggedAuxiliaryInjectRtxProbeWarning = true;
          Logger::warn(str::format(
            "D3D11: Auxiliary Remix pilot is probing injectRTX up to stage ",
            auxiliaryInjectRtxStageLimit,
            " before falling back to continuous geometry capture with scene-capture-only frame hooks."));
        }

        if (useAuxiliaryInjectRtxRequested
         && auxiliaryInjectRtxProbeAlreadyCompleted
         && !useAuxiliaryFullEndFrameAfterProbe
         && !m_loggedAuxiliaryInjectRtxProbeCompleteWarning) {
          m_loggedAuxiliaryInjectRtxProbeCompleteWarning = true;
          Logger::warn("D3D11: Auxiliary injectRTX probe already completed successfully on this run; keeping continuous DX11 geometry capture active while falling back to scene-capture-only frame hooks to preserve frame rate.");
        }

        if (shouldRunAuxiliaryInjectRtxAfterProbe && !m_loggedAuxiliaryInjectRtxAfterProbeWarning) {
          m_loggedAuxiliaryInjectRtxAfterProbeWarning = true;
          Logger::warn("D3D11: Auxiliary Remix pilot is running injectRTX once for each newly captured post-probe frame so DX11 ray tracing, lighting, and upscaling can be evaluated without repeating the same heavy work on every present.");
        }

        if (useAuxiliaryInjectRtxRequested
         && !useAuxiliaryInjectRtxProbe
         && !auxiliaryInjectRtxProbeAlreadyCompleted
         && !m_loggedAuxiliaryInjectRtxDisabledWarning) {
          m_loggedAuxiliaryInjectRtxDisabledWarning = true;
          Logger::warn("D3D11: Auxiliary injectRTX was requested, but it is currently disabled after reproducing a post-load crash on Days Gone. Falling back to the previously stable full endFrame path.");
        }

        const auto currentReflexFrameId = m_reflexFrameId;
        const bool useAuxiliaryInjectRtx = useAuxiliaryInjectRtxProbe || shouldRunAuxiliaryInjectRtxAfterProbe;
        const uint64_t auxiliaryInjectRtxCaptureFrame = m_lastAuxiliaryPilotCaptureFrame;
        auto emitAuxiliaryFullEndFrame = [this, currentReflexFrameId, targetImage, traceFrame, useAuxiliaryInjectRtx, useAuxiliaryInjectRtxProbe, auxiliaryInjectRtxCaptureFrame](DxvkContext* ctx) {
          if (traceFrame)
            D3D11EarlyTrace("D3D11Rtx::EndFrame auxiliary full CS begin");

          if (useAuxiliaryInjectRtxProbe)
            Logger::warn("D3D11: Auxiliary injectRTX probe CS begin.");

          RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::EndFrame auxiliary full end frame skipped because the command stream is not using an RTX context");
          if (rtxContext == nullptr)
            return;

          if (useAuxiliaryInjectRtxProbe)
            Logger::warn("D3D11: Auxiliary injectRTX probe before rtxContext->endFrame.");

          rtxContext->endFrame(currentReflexFrameId, targetImage, useAuxiliaryInjectRtx);

          if (useAuxiliaryInjectRtxProbe) {
            m_auxiliaryInjectRtxProbeCompleted.store(true, std::memory_order_relaxed);
          }

          if (!useAuxiliaryInjectRtxProbe && useAuxiliaryInjectRtx) {
            m_lastAuxiliaryInjectRtxCaptureFrame = auxiliaryInjectRtxCaptureFrame;
          }

          if (useAuxiliaryInjectRtxProbe)
            Logger::warn("D3D11: Auxiliary injectRTX probe after rtxContext->endFrame.");

          if (traceFrame)
            D3D11EarlyTrace("D3D11Rtx::EndFrame auxiliary full CS after rtxContext->endFrame");
        };

        logAuxiliaryInjectProbeStep("before EmitRtxCs");

        EmitRtxCs(std::move(emitAuxiliaryFullEndFrame));

        logAuxiliaryInjectProbeStep("after EmitRtxCs");
      } else {
        if (!m_loggedAuxiliarySceneCaptureEndFrameWarning) {
          m_loggedAuxiliarySceneCaptureEndFrameWarning = true;
          Logger::warn("D3D11: Auxiliary Remix pilot is running endFrameSceneCaptureOnly after successful geometry capture.");
        }

        auto emitSceneCaptureOnlyEndFrame = [traceFrame](DxvkContext* ctx) {
          if (traceFrame)
            D3D11EarlyTrace("D3D11Rtx::EndFrame auxiliary CS begin");

          RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::EndFrame auxiliary scene capture skipped because the command stream is not using an RTX context");
          if (rtxContext == nullptr)
            return;

          rtxContext->endFrameSceneCaptureOnly();

          if (traceFrame)
            D3D11EarlyTrace("D3D11Rtx::EndFrame auxiliary CS after endFrameSceneCaptureOnly");
        };

        EmitRtxCs(std::move(emitSceneCaptureOnlyEndFrame));
      }

      logAuxiliaryInjectProbeStep("before post-Emit SynchronizeRtxCs");

      SynchronizeRtxCs();

      logAuxiliaryInjectProbeStep("after post-Emit SynchronizeRtxCs");

      m_geometryCaptureFaultedThisFrame.store(false, std::memory_order_relaxed);
      m_hasProjectionMatrixThisFrame.store(false, std::memory_order_relaxed);
      m_auxiliaryPilotCapturesThisFrame.store(0u, std::memory_order_relaxed);
      m_pendingDrawCalls = 0;
      return;
    }

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::EndFrame after Flush");

    const bool shouldInjectRtx = callInjectRtx && m_hasProjectionMatrixThisFrame.load(std::memory_order_relaxed);

    if (callInjectRtx && !shouldInjectRtx && !m_loggedSkippedInjectWarning) {
      m_loggedSkippedInjectWarning = true;
      Logger::warn("D3D11: Skipping RTX end-of-frame injection until a projection matrix is inferred for the current frame.");
    }

    const auto currentReflexFrameId = m_reflexFrameId;
    auto emitEndFrame = [currentReflexFrameId, targetImage, shouldInjectRtx, traceFrame](DxvkContext* ctx) {
      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::EndFrame CS begin");

      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::EndFrame skipped because the command stream is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::EndFrame CS before rtxContext->endFrame");

      rtxContext->endFrame(currentReflexFrameId, targetImage, shouldInjectRtx);

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::EndFrame CS after rtxContext->endFrame");
    };

    context->EmitCs(std::move(emitEndFrame));

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::EndFrame EmitCs queued");

    m_geometryCaptureFaultedThisFrame.store(false, std::memory_order_relaxed);
    m_hasProjectionMatrixThisFrame.store(false, std::memory_order_relaxed);
    m_pendingDrawCalls = 0;
  }


  void D3D11Rtx::OnPresent(
          D3D11ImmediateContext*      context,
    const Rc<DxvkImage>&              targetImage) {
    if (!m_enabled || !CanUseRtxExecutionContext())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    D3D10DeviceLock contextLock = context->LockContext();

    const bool traceFrame = m_reflexFrameId < 8;
    const bool useAuxiliaryOnPresent = !m_parent->UsesImmediateContextRtx();

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::OnPresent enter");

    if (useAuxiliaryOnPresent && !m_loggedAuxiliaryOnPresentWarning) {
      m_loggedAuxiliaryOnPresentWarning = true;
      Logger::warn("D3D11: Auxiliary Remix pilot is running OnPresent after successful scene capture.");
    }

    auto emitOnPresent = [targetImage, traceFrame](DxvkContext* ctx) {
      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::OnPresent CS begin");

      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::OnPresent skipped because the command stream is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::OnPresent CS before rtxContext->onPresent");

      rtxContext->onPresent(targetImage);

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::OnPresent CS after rtxContext->onPresent");
    };

    if (useAuxiliaryOnPresent) {
      context->Flush();
      SynchronizeRtxCs();
      EmitRtxCs(std::move(emitOnPresent));
      SynchronizeRtxCs();
    } else {
      context->EmitCs(std::move(emitOnPresent));
    }

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::OnPresent EmitCs queued");

    m_reflexFrameId += 1;
  }


  void D3D11Rtx::AdvanceFrameIdForPresentBypass() {
    if (!m_enabled || !CanUseRtxExecutionContext())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_geometryCaptureFaultedThisFrame.store(false, std::memory_order_relaxed);
    m_hasProjectionMatrixThisFrame.store(false, std::memory_order_relaxed);
    m_auxiliaryPilotCapturesThisFrame.store(0u, std::memory_order_relaxed);
    m_pendingDrawCalls = 0;
    m_reflexFrameId += 1;
  }

}