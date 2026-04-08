#include <unordered_map>

#include "d3d11_options.h"

namespace dxvk {
  
  D3D11Options::D3D11Options(const Config& config, const Rc<DxvkDevice>& device) {
    const DxvkDeviceInfo& devInfo = device->properties();
    const bool enableRemix = config.getOption<bool>("d3d11.enableRemix", false);

    this->enableRemix           = enableRemix;
    this->useRtxContext         = config.getOption<bool>("d3d11.useRtxContext", enableRemix);
    this->remixPilotCaptureInterval = config.getOption<int32_t>("d3d11.remixPilotCaptureInterval", 0);
    this->remixPilotProbeCaptureInterval = config.getOption<int32_t>("d3d11.remixPilotProbeCaptureInterval", 30);
    this->remixPilotPostProbeCaptureInterval = config.getOption<int32_t>("d3d11.remixPilotPostProbeCaptureInterval", 10);
    this->remixPilotMaxCapturesPerFrame = config.getOption<int32_t>("d3d11.remixPilotMaxCapturesPerFrame", 64);
    this->remixPilotProbeMaxCapturesPerFrame = config.getOption<int32_t>("d3d11.remixPilotProbeMaxCapturesPerFrame", 1);
    // NV-DXVK start: Default DX11 Remix to the validated auxiliary post-probe path
    this->remixPilotPostProbeMaxCapturesPerFrame = config.getOption<int32_t>("d3d11.remixPilotPostProbeMaxCapturesPerFrame", enableRemix ? 1 : 4);
    this->remixPilotMaxSuccessfulCaptures = config.getOption<int32_t>("d3d11.remixPilotMaxSuccessfulCaptures", 0);
    this->remixPilotEnableSceneCaptureEndFrame = config.getOption<bool>("d3d11.remixPilotEnableSceneCaptureEndFrame", false);
    this->remixPilotEnableResetScreenResolution = config.getOption<bool>("d3d11.remixPilotEnableResetScreenResolution", false);
    this->remixPilotEnableOnPresent = config.getOption<bool>("d3d11.remixPilotEnableOnPresent", false);
    this->remixPilotEnableFullEndFrame = config.getOption<bool>("d3d11.remixPilotEnableFullEndFrame", enableRemix);
    this->remixPilotEnableFullEndFrameAfterProbe = config.getOption<bool>("d3d11.remixPilotEnableFullEndFrameAfterProbe", enableRemix);
    this->remixPilotEnableInjectRtx = config.getOption<bool>("d3d11.remixPilotEnableInjectRtx", enableRemix);
    this->remixPilotEnableInjectRtxAfterProbe = config.getOption<bool>("d3d11.remixPilotEnableInjectRtxAfterProbe", enableRemix);
    this->remixPilotInjectRtxStageLimit = config.getOption<int32_t>("d3d11.remixPilotInjectRtxStageLimit", enableRemix ? 15 : 0);
    // NV-DXVK end
    this->dcSingleUseMode       = config.getOption<bool>("d3d11.dcSingleUseMode", true);
    this->enableRtOutputNanFixup   = config.getOption<bool>("d3d11.enableRtOutputNanFixup", false);
    this->zeroInitWorkgroupMemory  = config.getOption<bool>("d3d11.zeroInitWorkgroupMemory", false);
    this->forceTgsmBarriers     = config.getOption<bool>("d3d11.forceTgsmBarriers", false);
    this->relaxedBarriers       = config.getOption<bool>("d3d11.relaxedBarriers", false);
    this->ignoreGraphicsBarriers = config.getOption<bool>("d3d11.ignoreGraphicsBarriers", false);
    this->maxTessFactor         = config.getOption<int32_t>("d3d11.maxTessFactor", 0);
    this->samplerAnisotropy     = config.getOption<int32_t>("d3d11.samplerAnisotropy", -1);
    this->invariantPosition     = config.getOption<bool>("d3d11.invariantPosition", true);
    this->floatControls         = config.getOption<bool>("d3d11.floatControls", true);
    this->disableMsaa           = config.getOption<bool>("d3d11.disableMsaa", false);
    this->deferSurfaceCreation  = config.getOption<bool>("dxgi.deferSurfaceCreation", false);
    this->numBackBuffers        = config.getOption<int32_t>("dxgi.numBackBuffers", 0);
    this->maxFrameLatency       = config.getOption<int32_t>("dxgi.maxFrameLatency", 0);
    this->maxFrameRate          = config.getOption<int32_t>("dxgi.maxFrameRate", 0);
    this->syncInterval          = config.getOption<int32_t>("dxgi.syncInterval", -1);
    this->tearFree              = config.getOption<Tristate>("dxgi.tearFree", Tristate::Auto);

    this->constantBufferRangeCheck = config.getOption<bool>("d3d11.constantBufferRangeCheck", false)
      && DxvkGpuVendor(devInfo.core.properties.vendorID) != DxvkGpuVendor::Amd;

    bool apitraceAttached = false;
    apitraceAttached = ::GetModuleHandle("dxgitrace.dll") != nullptr;

    this->apitraceMode = config.getOption<bool>("d3d11.apitraceMode", apitraceAttached);

    // Inform user in case they have the option enabled or a game
    // ships a file called dxgitrace.dll for whatever reason.
    if (this->apitraceMode)
      Logger::warn("D3D11: Apitrace mode enabled, may affect performance!");
  }
  
}