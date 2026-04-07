#include "d3d11_rtx.h"

#include <algorithm>
#include <cfloat>
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

namespace dxvk {

  namespace {

    struct D3D11RtxResolvedAttribute {
      DxvkVertexAttribute attribute;
      const D3D11VertexBufferBinding* binding = nullptr;
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

  namespace {
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
  }


  void D3D11Rtx::NotifyDraw(const DrawContext& drawContext) {
    if (!m_enabled || !m_parent->UsesImmediateContextRtx() || m_geometryCaptureDisabled.load(std::memory_order_relaxed))
      return;

    std::lock_guard<std::mutex> lock(m_mutex);

    m_pendingDrawCalls += 1;

    if (!m_loggedExperimentalWarning) {
      m_loggedExperimentalWarning = true;
      Logger::warn("D3D11: Experimental Remix path enabled. Indexed immediate-context geometry capture is active for supported triangle draws.");
    }

    (void)drawContext;
  }


  void D3D11Rtx::CommitGeometryToRT(
          D3D11DeviceContext*         context,
    const DrawContext&                drawContext) {
    if (!m_enabled || !m_parent->UsesImmediateContextRtx() || m_geometryCaptureDisabled.load(std::memory_order_relaxed))
      return;

    std::lock_guard<std::mutex> lock(m_mutex);

    DrawContext resolvedDrawContext = drawContext;
    if (!resolveIndirectDrawContext(resolvedDrawContext))
      return;

    auto& iaState = context->m_state.ia;

    if (iaState.inputLayout == nullptr)
      return;

    VkPrimitiveTopology vkTopology = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    if (!isSupportedTopology(iaState.primitiveTopology, vkTopology))
      return;

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
    drawCallState.transformData.worldToView = Matrix4();
    drawCallState.transformData.objectToView = Matrix4();
    drawCallState.transformData.viewToProjection = Matrix4();

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

    context->EmitCs([this, params, drawCallState = std::move(drawCallState)](DxvkContext* ctx) mutable {
      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::CommitGeometryToRT skipped because CS thread is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      if (!TryCommitGeometryToRt(rtxContext, params, drawCallState)) {
        if (!m_geometryCaptureDisabled.exchange(true, std::memory_order_relaxed)) {
          Logger::warn("D3D11: Disabled Remix geometry capture after backend fault in commitGeometryToRT.");
        }
      }
    });
  }


  void D3D11Rtx::ResetScreenResolution(
          D3D11ImmediateContext*      context,
          uint32_t                    width,
          uint32_t                    height) {
    if (!m_enabled || !m_parent->UsesImmediateContextRtx())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    D3D10DeviceLock contextLock = context->LockContext();

    if (m_screenWidth == width && m_screenHeight == height)
      return;

    m_screenWidth = width;
    m_screenHeight = height;

    context->EmitCs([cWidth = width, cHeight = height](DxvkContext* ctx) {
      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::ResetScreenResolution skipped because CS thread is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      rtxContext->resetScreenResolution({ cWidth, cHeight, 1u });
    });
  }


  void D3D11Rtx::EndFrame(
          D3D11ImmediateContext*      context,
    const Rc<DxvkImage>&              targetImage,
          bool                        callInjectRtx) {
    if (!m_enabled || !m_parent->UsesImmediateContextRtx())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    D3D10DeviceLock contextLock = context->LockContext();

    const bool traceFrame = m_reflexFrameId < 8;

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::EndFrame enter");

    if (!m_parent->GetOptions()->enableRemix) {
      context->Flush();
    }

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::EndFrame after Flush");

    const auto currentReflexFrameId = m_reflexFrameId;
    context->EmitCs([currentReflexFrameId, targetImage, callInjectRtx, traceFrame](DxvkContext* ctx) {
      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::EndFrame CS begin");

      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::EndFrame skipped because CS thread is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::EndFrame CS before rtxContext->endFrame");

      rtxContext->endFrame(currentReflexFrameId, targetImage, callInjectRtx);

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::EndFrame CS after rtxContext->endFrame");
    });

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::EndFrame EmitCs queued");

    m_pendingDrawCalls = 0;
  }


  void D3D11Rtx::OnPresent(
          D3D11ImmediateContext*      context,
    const Rc<DxvkImage>&              targetImage) {
    if (!m_enabled || !m_parent->UsesImmediateContextRtx())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    D3D10DeviceLock contextLock = context->LockContext();

    const bool traceFrame = m_reflexFrameId < 8;

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::OnPresent enter");

    context->EmitCs([targetImage, traceFrame](DxvkContext* ctx) {
      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::OnPresent CS begin");

      RtxContext* rtxContext = getRtxContextOrTrace(ctx, "D3D11Rtx::OnPresent skipped because CS thread is not using an RTX context");
      if (rtxContext == nullptr)
        return;

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::OnPresent CS before rtxContext->onPresent");

      rtxContext->onPresent(targetImage);

      if (traceFrame)
        D3D11EarlyTrace("D3D11Rtx::OnPresent CS after rtxContext->onPresent");
    });

    if (traceFrame)
      D3D11EarlyTrace("D3D11Rtx::OnPresent EmitCs queued");

    m_reflexFrameId += 1;
  }


  void D3D11Rtx::AdvanceFrameIdForPresentBypass() {
    if (!m_enabled || !m_parent->UsesImmediateContextRtx())
      return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_reflexFrameId += 1;
  }

}