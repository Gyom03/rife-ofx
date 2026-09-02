#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxGPURender.h>
#include <ofxParam.h>

#if RIFE_ENABLE_INFERENCE
#include "RifeEngine.h"
#endif
#include "ModelRegistry.h"
#include "TemporalFrameProvider.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <cmath>
#include <memory>
#include <limits>
#include <sstream>
#include <vector>

namespace {

constexpr char kPluginIdentifier[] = "com.rifeofx.RifeFrameInterpolator";
constexpr int kPluginMajor = 0;
constexpr int kPluginMinor = 1;
constexpr char kEnabledParam[] = "enabled";
constexpr char kInterpolationAmountParam[] = "interpolationAmount";
constexpr char kAutoTimingParam[] = "autoTiming";
constexpr char kModeParam[] = "mode";
constexpr char kQualityParam[] = "quality";
constexpr char kDebugParam[] = "debug";
constexpr char kModelParam[] = "model";
constexpr char kGpuDeviceParam[] = "gpuDevice";
constexpr int kFramesBefore = 1;
constexpr int kFramesAfter = 2;
constexpr const char* kModelIds[] = {
    "rife-v4.6",       "rife-v4.22-lite", "rife-v4.25-lite",
    "rife-v4.25",      "rife-v4.26",      "rife-v4.26-large"};
constexpr const char* kModelLabels[] = {
    "RIFE 4.6",       "RIFE 4.22 Lite", "RIFE 4.25 Lite",
    "RIFE 4.25",      "RIFE 4.26",      "RIFE 4.26 Large"};
constexpr int kModelCount = static_cast<int>(sizeof(kModelIds) / sizeof(kModelIds[0]));
constexpr int kQualityModelIndexes[] = {1, 2, 3, 5};
constexpr const char* kQualityLabels[] = {"Fast", "Balanced", "High", "Maximum"};

OfxHost* gHost = nullptr;
const OfxPropertySuiteV1* gPropertySuite = nullptr;
const OfxImageEffectSuiteV1* gImageEffectSuite = nullptr;
const OfxParameterSuiteV1* gParameterSuite = nullptr;

struct InstanceData {
  OfxImageClipHandle sourceClip = nullptr;
  OfxImageClipHandle outputClip = nullptr;
  OfxParamHandle enabledParam = nullptr;
  OfxParamHandle interpolationAmountParam = nullptr;
  OfxParamHandle autoTimingParam = nullptr;
  OfxParamHandle modeParam = nullptr;
  OfxParamHandle qualityParam = nullptr;
  OfxParamHandle debugParam = nullptr;
  OfxParamHandle modelParam = nullptr;
  OfxParamHandle gpuDeviceParam = nullptr;
  std::unique_ptr<rifeofx::TemporalFrameProvider> temporalProvider;
  double sourceFrameRate = 0.0;
  double outputFrameRate = 0.0;
  bool frameRatesAvailable = false;
  OfxTime sourceFirstFrame = 0.0;
  OfxTime sourceLastFrame = 0.0;
  bool sourceFrameRangeAvailable = false;
#if RIFE_ENABLE_INFERENCE
  std::filesystem::path modelRoot;
  std::unique_ptr<rifeofx::RifeEngine> rifeEngine;
#endif
};

std::filesystem::path pluginModelsRoot() {
  HMODULE module = nullptr;
  const BOOL found = GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      reinterpret_cast<LPCWSTR>(&pluginModelsRoot), &module);
  if (!found || !module) {
    return {};
  }

  wchar_t modulePath[32768] = {};
  const DWORD length = GetModuleFileNameW(module, modulePath,
                                          static_cast<DWORD>(sizeof(modulePath) /
                                                             sizeof(modulePath[0])));
  if (length == 0 || length >= sizeof(modulePath) / sizeof(modulePath[0])) {
    return {};
  }
  const std::filesystem::path win64Directory(modulePath);
  return win64Directory.parent_path().parent_path() / L"Resources" / L"models";
}

void debugLog(const std::string& message);

std::filesystem::path configuredModelsRoot(
    const std::filesystem::path& bundleModelsRoot) {
  // Model weights are intentionally external: the repository does not make a
  // redistribution claim for third-party RIFE weights. The manifest remains
  // bundled, while this variable points at a user-installed model directory.
  wchar_t* configured = _wgetenv(L"RIFEOFX_MODELS_ROOT");
  if (configured && *configured) {
    const std::filesystem::path externalRoot(configured);
    if (std::filesystem::is_directory(externalRoot)) {
      return externalRoot;
    }
    debugLog("RIFEOFX_MODELS_ROOT is not a directory; using bundle models root");
  }
  return bundleModelsRoot;
}

void debugLog(const std::string& message) {
  OutputDebugStringA(("[RifeOFX] " + message + "\n").c_str());
}

void logHostInt(const char* property, const char* label) {
  int value = 0;
  if (gPropertySuite->propGetInt(gHost->host, property, 0, &value) == kOfxStatOK) {
    std::ostringstream stream;
    stream << "host " << label << "=" << value;
    debugLog(stream.str());
  }
}

OfxStatus fetchSuites(OfxHost* host) {
  if (!host || !host->fetchSuite) {
    return kOfxStatErrMissingHostFeature;
  }

  gHost = host;
  gPropertySuite = static_cast<const OfxPropertySuiteV1*>(
      host->fetchSuite(host->host, kOfxPropertySuite, 1));
  gImageEffectSuite = static_cast<const OfxImageEffectSuiteV1*>(
      host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
  gParameterSuite = static_cast<const OfxParameterSuiteV1*>(
      host->fetchSuite(host->host, kOfxParameterSuite, 1));

  if (!gPropertySuite || !gImageEffectSuite || !gParameterSuite) {
    return kOfxStatErrMissingHostFeature;
  }
  return kOfxStatOK;
}

void setString(OfxPropertySetHandle props, const char* name, int index,
               const char* value) {
  gPropertySuite->propSetString(props, name, index, value);
}

void setInt(OfxPropertySetHandle props, const char* name, int index, int value) {
  gPropertySuite->propSetInt(props, name, index, value);
}

bool getBoolParam(InstanceData* data, OfxTime time) {
  int enabled = 1;
  if (!data || !data->enabledParam ||
      gParameterSuite->paramGetValueAtTime(data->enabledParam, time, &enabled) != kOfxStatOK) {
    return true;
  }
  return enabled != 0;
}

bool getDebugParam(InstanceData* data, OfxTime time) {
  int debug = 0;
  if (!data || !data->debugParam ||
      gParameterSuite->paramGetValueAtTime(data->debugParam, time, &debug) != kOfxStatOK) {
    return false;
  }
  return debug != 0;
}

double getAmountParam(const InstanceData* data, OfxTime time) {
  double amount = 0.5;
  if (!data || !data->interpolationAmountParam ||
      gParameterSuite->paramGetValueAtTime(data->interpolationAmountParam, time, &amount) != kOfxStatOK) {
    return amount;
  }
  return std::clamp(amount, 0.0, 1.0);
}

bool getAutoTimingParam(const InstanceData* data, OfxTime time) {
  int autoTiming = 1;
  if (!data || !data->autoTimingParam ||
      gParameterSuite->paramGetValueAtTime(data->autoTimingParam, time,
                                            &autoTiming) != kOfxStatOK) {
    return true;
  }
  return autoTiming != 0;
}

int getChoiceParam(OfxParamHandle parameter, OfxTime time, int fallback) {
  int value = fallback;
  if (!parameter ||
      gParameterSuite->paramGetValueAtTime(parameter, time, &value) != kOfxStatOK) {
    return fallback;
  }
  return value;
}

std::string getSelectedModelId(const InstanceData* data, OfxTime time) {
  const int modelIndex = std::clamp(
      getChoiceParam(data ? data->modelParam : nullptr, time, 0), 0, kModelCount - 1);
  const int mode = getChoiceParam(data ? data->modeParam : nullptr, time, 1);
  if (mode == 0) {
    const int qualityIndex = std::clamp(
        getChoiceParam(data ? data->qualityParam : nullptr, time, 1), 0, 3);
    return kModelIds[kQualityModelIndexes[qualityIndex]];
  }
  return kModelIds[modelIndex];
}

bool readFrameRate(OfxPropertySetHandle properties, double* frameRate) {
  return properties && frameRate &&
         gPropertySuite->propGetDouble(properties, kOfxImageEffectPropFrameRate,
                                       0, frameRate) == kOfxStatOK &&
         *frameRate > 0.0;
}

bool isUsableFrameRange(const double range[2]) {
  if (!range || !std::isfinite(range[0]) || !std::isfinite(range[1]) ||
      range[0] > range[1]) {
    return false;
  }

  // Resolve may expose an INT_MIN-like sentinel for an unbounded Fusion
  // source instead of a real clip frame range. Never use that value to clamp
  // a temporal request.
  constexpr double kFrameSentinelLimit =
      static_cast<double>(std::numeric_limits<int>::max()) / 16.0;
  return range[0] > -kFrameSentinelLimit && range[1] < kFrameSentinelLimit;
}

struct RenderTiming {
  OfxTime sourceFrame = 0.0;
  float timestep = 0.0f;
};

RenderTiming getRenderTiming(const InstanceData* data, OfxTime outputTime) {
  RenderTiming timing;
  const bool autoTiming = getAutoTimingParam(data, outputTime);
  if (autoTiming && data && data->frameRatesAvailable) {
    const double sourcePosition =
        outputTime * data->sourceFrameRate / data->outputFrameRate;
    const double sourceFrame = std::floor(sourcePosition);
    timing.sourceFrame = static_cast<OfxTime>(sourceFrame);
    timing.timestep = static_cast<float>(
        std::clamp(sourcePosition - sourceFrame, 0.0, 1.0));
  } else {
    timing.sourceFrame = static_cast<OfxTime>(std::floor(outputTime));
    timing.timestep = static_cast<float>(
        std::clamp(getAmountParam(data, outputTime), 0.0, 1.0));
  }
  return timing;
}

OfxStatus describe(OfxImageEffectHandle effect) {
  const OfxStatus suiteStatus = fetchSuites(gHost);
  if (suiteStatus != kOfxStatOK) {
    return suiteStatus;
  }

  OfxPropertySetHandle effectProps = nullptr;
  OfxStatus status = gImageEffectSuite->getPropertySet(effect, &effectProps);
  if (status != kOfxStatOK) {
    return status;
  }

  setString(effectProps, kOfxPropLabel, 0, "RIFE Frame Interpolator");
  setString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "Open Source AI");
  setString(effectProps, kOfxImageEffectPropSupportedContexts, 0,
           kOfxImageEffectContextFilter);
  setString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0,
           kOfxBitDepthFloat);
  setInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
  setInt(effectProps, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
  setInt(effectProps, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
  setInt(effectProps, kOfxImageEffectPropSupportsTiles, 0, 0);
  setInt(effectProps, kOfxImageEffectPropTemporalClipAccess, 0, 1);
  setString(effectProps, kOfxImageEffectPluginRenderThreadSafety, 0,
           kOfxImageEffectRenderInstanceSafe);

  // Resolve's actual host capabilities are queried here for diagnostics only.
  // No capability is assumed for later temporal/GPU stages.
  logHostInt(kOfxImageEffectPropTemporalClipAccess, "temporalClipAccess");
  logHostInt(kOfxImageEffectPropSupportsTiles, "supportsTiles");
  logHostInt(kOfxImageEffectPropSupportsMultiResolution, "supportsMultiResolution");
  logHostInt(kOfxImageEffectPropSupportsMultipleClipDepths, "supportsMultipleClipDepths");
  logHostInt(kOfxImageEffectPropCudaRenderSupported, "cudaRenderSupported");
  logHostInt(kOfxImageEffectPropOpenCLRenderSupported, "openclRenderSupported");
  char* hostName = nullptr;
  char* hostVersion = nullptr;
  if (gPropertySuite->propGetString(gHost->host, kOfxPropName, 0, &hostName) == kOfxStatOK &&
      hostName) {
    debugLog(std::string("host name=") + hostName);
  }
  if (gPropertySuite->propGetString(gHost->host, kOfxPropVersion, 0, &hostVersion) == kOfxStatOK &&
      hostVersion) {
    debugLog(std::string("host version=") + hostVersion);
  }

  return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  char* context = nullptr;
  if (gPropertySuite->propGetString(inArgs, kOfxImageEffectPropContext, 0, &context) != kOfxStatOK ||
      !context || std::strcmp(context, kOfxImageEffectContextFilter) != 0) {
    return kOfxStatReplyDefault;
  }

  OfxPropertySetHandle clipProps = nullptr;
  OfxStatus status = gImageEffectSuite->clipDefine(
      effect, kOfxImageEffectOutputClipName, &clipProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(clipProps, kOfxImageEffectPropSupportedComponents, 0,
            kOfxImageComponentRGBA);
  setString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0,
            kOfxBitDepthFloat);

  status = gImageEffectSuite->clipDefine(
      effect, kOfxImageEffectSimpleSourceClipName, &clipProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(clipProps, kOfxImageEffectPropSupportedComponents, 0,
            kOfxImageComponentRGBA);
  setString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0,
            kOfxBitDepthFloat);
  setInt(clipProps, kOfxImageClipPropOptional, 0, 0);
  setInt(clipProps, kOfxImageEffectPropTemporalClipAccess, 0, 1);

  OfxParamSetHandle paramSet = nullptr;
  status = gImageEffectSuite->getParamSet(effect, &paramSet);
  if (status != kOfxStatOK) {
    return status;
  }

  OfxPropertySetHandle paramProps = nullptr;
  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeBoolean,
                                        kEnabledParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Enabled");
  setString(paramProps, kOfxParamPropScriptName, 0, kEnabledParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Enable or disable the prototype passthrough render");
  setInt(paramProps, kOfxParamPropDefault, 0, 1);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeDouble,
                                        kInterpolationAmountParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Interpolation Amount");
  setString(paramProps, kOfxParamPropScriptName, 0, kInterpolationAmountParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Blend between the previous and next source frames");
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDefault, 0, 0.5);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropMin, 0, 0.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropMax, 0, 1.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDisplayMin, 0, 0.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDisplayMax, 0, 1.0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeBoolean,
                                        kAutoTimingParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Auto Timing");
  setString(paramProps, kOfxParamPropScriptName, 0, kAutoTimingParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Derive the RIFE timestep from source and output frame rates");
  setInt(paramProps, kOfxParamPropDefault, 0, 1);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeChoice,
                                        kModeParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Mode");
  setString(paramProps, kOfxParamPropScriptName, 0, kModeParam);
  setString(paramProps, kOfxParamPropChoiceOption, 0, "Quality");
  setString(paramProps, kOfxParamPropChoiceOption, 1, "Advanced");
  setInt(paramProps, kOfxParamPropDefault, 0, 1);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeChoice,
                                        kQualityParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Quality");
  setString(paramProps, kOfxParamPropScriptName, 0, kQualityParam);
  for (int index = 0; index < 4; ++index) {
    setString(paramProps, kOfxParamPropChoiceOption, index,
              kQualityLabels[index]);
  }
  setInt(paramProps, kOfxParamPropDefault, 0, 1);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeBoolean,
                                        kDebugParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Debug");
  setString(paramProps, kOfxParamPropScriptName, 0, kDebugParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Write temporal frame timestamps to the debugger");
  setInt(paramProps, kOfxParamPropDefault, 0, 0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeChoice,
                                        kModelParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Model");
  setString(paramProps, kOfxParamPropScriptName, 0, kModelParam);
  for (int index = 0; index < kModelCount; ++index) {
    setString(paramProps, kOfxParamPropChoiceOption, index,
              kModelLabels[index]);
  }
  setInt(paramProps, kOfxParamPropDefault, 0, 0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeChoice,
                                        kGpuDeviceParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "GPU Device");
  setString(paramProps, kOfxParamPropScriptName, 0, kGpuDeviceParam);
  setString(paramProps, kOfxParamPropChoiceOption, 0, "Vulkan Device 0");
  setInt(paramProps, kOfxParamPropDefault, 0, 0);

  return kOfxStatOK;
}

OfxStatus createInstance(OfxImageEffectHandle effect) {
  auto* data = new InstanceData();
  OfxStatus status = gImageEffectSuite->clipGetHandle(
      effect, kOfxImageEffectSimpleSourceClipName, &data->sourceClip, nullptr);
  if (status != kOfxStatOK) {
    delete data;
    return status;
  }
  status = gImageEffectSuite->clipGetHandle(
      effect, kOfxImageEffectOutputClipName, &data->outputClip, nullptr);
  if (status != kOfxStatOK) {
    delete data;
    return status;
  }

  OfxParamSetHandle paramSet = nullptr;
  status = gImageEffectSuite->getParamSet(effect, &paramSet);
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kEnabledParam,
                                             &data->enabledParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kInterpolationAmountParam,
                                             &data->interpolationAmountParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kAutoTimingParam,
                                             &data->autoTimingParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kModeParam,
                                             &data->modeParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kQualityParam,
                                             &data->qualityParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kDebugParam,
                                             &data->debugParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kModelParam,
                                             &data->modelParam, nullptr);
  }
  if (status == kOfxStatOK) {
    status = gParameterSuite->paramGetHandle(paramSet, kGpuDeviceParam,
                                             &data->gpuDeviceParam, nullptr);
  }
  if (status == kOfxStatOK) {
    OfxPropertySetHandle sourceProperties = nullptr;
    OfxPropertySetHandle effectProperties = nullptr;
    const OfxStatus sourceStatus = gImageEffectSuite->clipGetPropertySet(
        data->sourceClip, &sourceProperties);
    OfxStatus effectStatus = kOfxStatFailed;
    if (sourceStatus == kOfxStatOK) {
      effectStatus = gImageEffectSuite->getPropertySet(effect, &effectProperties);
    }
    data->frameRatesAvailable =
        sourceStatus == kOfxStatOK && effectStatus == kOfxStatOK &&
        readFrameRate(sourceProperties, &data->sourceFrameRate) &&
        readFrameRate(effectProperties, &data->outputFrameRate);
    if (data->frameRatesAvailable) {
      std::ostringstream stream;
      stream << "frame rates source=" << data->sourceFrameRate
             << " output=" << data->outputFrameRate;
      debugLog(stream.str());
    } else {
      debugLog("frame rates unavailable; Auto Timing will use manual Amount");
    }

    double sourceRange[2] = {};
    data->sourceFrameRangeAvailable =
        sourceStatus == kOfxStatOK &&
        gPropertySuite->propGetDoubleN(sourceProperties,
                                       kOfxImageEffectPropFrameRange, 2,
                                       sourceRange) == kOfxStatOK &&
        isUsableFrameRange(sourceRange);
    if (data->sourceFrameRangeAvailable) {
      data->sourceFirstFrame = sourceRange[0];
      data->sourceLastFrame = sourceRange[1];
      std::ostringstream stream;
      stream << "source frame range=" << data->sourceFirstFrame << ".."
             << data->sourceLastFrame;
      debugLog(stream.str());
    } else {
      debugLog("source frame range unavailable or sentinel; temporal edge clamping disabled");
    }
  }
  if (status == kOfxStatOK) {
    data->temporalProvider = std::make_unique<rifeofx::TemporalFrameProvider>(
        gPropertySuite, gImageEffectSuite, data->sourceClip, effect, false);
    if (data->sourceFrameRangeAvailable) {
      data->temporalProvider->setFrameRange(data->sourceFirstFrame,
                                            data->sourceLastFrame);
    }
#if RIFE_ENABLE_INFERENCE
    const std::filesystem::path bundleModelsRoot = pluginModelsRoot();
    data->modelRoot = configuredModelsRoot(bundleModelsRoot);
    data->rifeEngine = std::make_unique<rifeofx::RifeEngine>(
        0, data->modelRoot, bundleModelsRoot / "registry.csv");
#endif
  }

  // The instance data belongs in the effect's private data property set.
  // Retrieve it through the image-effect suite rather than relying on a host ABI.
  OfxPropertySetHandle effectProps = nullptr;
  if (status == kOfxStatOK) {
    status = gImageEffectSuite->getPropertySet(effect, &effectProps);
  }
  if (status == kOfxStatOK) {
    status = gPropertySuite->propSetPointer(effectProps, kOfxPropInstanceData, 0, data);
  }
  if (status != kOfxStatOK) {
    delete data;
  }
  return status;
}

InstanceData* getInstanceData(OfxImageEffectHandle effect) {
  OfxPropertySetHandle props = nullptr;
  void* pointer = nullptr;
  if (!gImageEffectSuite || gImageEffectSuite->getPropertySet(effect, &props) != kOfxStatOK ||
      !props || gPropertySuite->propGetPointer(props, kOfxPropInstanceData, 0, &pointer) != kOfxStatOK) {
    return nullptr;
  }
  return static_cast<InstanceData*>(pointer);
}

OfxStatus destroyInstance(OfxImageEffectHandle effect) {
  InstanceData* data = getInstanceData(effect);
  delete data;
  return kOfxStatOK;
}

OfxStatus isIdentity(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs,
                     OfxPropertySetHandle outArgs) {
  InstanceData* data = getInstanceData(effect);
  if (!data) {
    return kOfxStatErrBadHandle;
  }

  OfxTime time = 0.0;
  if (gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time) != kOfxStatOK) {
    return kOfxStatReplyDefault;
  }

  if (!getBoolParam(data, time)) {
    gPropertySuite->propSetString(outArgs, kOfxPropName, 0,
                                  kOfxImageEffectSimpleSourceClipName);
    gPropertySuite->propSetDouble(outArgs, kOfxPropTime, 0, time);
    return kOfxStatOK;
  }
  return kOfxStatReplyDefault;
}

OfxStatus getFramesNeeded(OfxImageEffectHandle effect,
                          OfxPropertySetHandle inArgs,
                          OfxPropertySetHandle outArgs) {
  if (!getInstanceData(effect)) {
    return kOfxStatErrBadHandle;
  }

  OfxTime time = 0.0;
  if (gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time) != kOfxStatOK) {
    return kOfxStatErrBadHandle;
  }

  const InstanceData* data = getInstanceData(effect);
  const RenderTiming timing = getRenderTiming(data, time);

  // Request four consecutive source frames: F[n-1], F[n], F[n+1], F[n+2].
  double sourceRange[2] = {
      timing.sourceFrame - static_cast<OfxTime>(kFramesBefore),
      timing.sourceFrame + static_cast<OfxTime>(kFramesAfter),
  };
  if (data->sourceFrameRangeAvailable) {
    sourceRange[0] = std::max(sourceRange[0], data->sourceFirstFrame);
    sourceRange[1] = std::min(sourceRange[1], data->sourceLastFrame);
  }
  const OfxStatus status = gPropertySuite->propSetDoubleN(
      outArgs, "OfxImageClipPropFrameRange_Source", 2, sourceRange);
  return status == kOfxStatOK ? kOfxStatOK : status;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  InstanceData* data = getInstanceData(effect);
  if (!data || !data->temporalProvider) {
    return kOfxStatErrBadHandle;
  }

  OfxTime time = 0.0;
  OfxRectI renderWindow{};
  if (gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time) != kOfxStatOK ||
      gPropertySuite->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4,
                                  &renderWindow.x1) != kOfxStatOK) {
    return kOfxStatErrBadHandle;
  }

  const RenderTiming timing = getRenderTiming(data, time);
  data->temporalProvider->setDebug(getDebugParam(data, time));
  std::vector<const rifeofx::CachedFrame*> frames;
  OfxStatus status = data->temporalProvider->getTemporalWindow(
      timing.sourceFrame, kFramesBefore, kFramesAfter, frames);
  if (status != kOfxStatOK || frames.size() != kFramesBefore + kFramesAfter + 1) {
    return gImageEffectSuite->abort(effect)
               ? kOfxStatOK
               : (status == kOfxStatOK ? kOfxStatFailed : status);
  }

  OfxPropertySetHandle outputImage = nullptr;
  status = gImageEffectSuite->clipGetImage(data->outputClip, time, nullptr, &outputImage);
  if (status != kOfxStatOK || !outputImage) {
    return gImageEffectSuite->abort(effect)
               ? kOfxStatOK
               : (status == kOfxStatOK ? kOfxStatFailed : status);
  }

  void* outputData = nullptr;
  int outputRowBytes = 0;
  OfxRectI outputBounds{};
  char* outputComponents = nullptr;
  char* outputDepth = nullptr;
  const bool valid =
      gPropertySuite->propGetPointer(outputImage, kOfxImagePropData, 0, &outputData) == kOfxStatOK &&
      gPropertySuite->propGetInt(outputImage, kOfxImagePropRowBytes, 0, &outputRowBytes) == kOfxStatOK &&
      gPropertySuite->propGetIntN(outputImage, kOfxImagePropBounds, 4, &outputBounds.x1) == kOfxStatOK &&
      gPropertySuite->propGetString(outputImage, kOfxImageEffectPropComponents, 0,
                                    &outputComponents) == kOfxStatOK &&
      gPropertySuite->propGetString(outputImage, kOfxImageEffectPropPixelDepth, 0,
                                    &outputDepth) == kOfxStatOK;

  if (!valid || !outputData || !outputComponents || !outputDepth ||
      std::strcmp(outputComponents, kOfxImageComponentRGBA) != 0 ||
      std::strcmp(outputDepth, kOfxBitDepthFloat) != 0) {
    gImageEffectSuite->clipReleaseImage(outputImage);
    return kOfxStatErrImageFormat;
  }

  // Window order is [F[n-1], F[n], F[n+1], F[n+2]]. RIFE consumes the
  // consecutive pair F[n] / F[n+1]; the outer frames remain available for
  // future scene/motion consistency checks.
  const rifeofx::CachedFrame& frameA = *frames[static_cast<size_t>(kFramesBefore)];
  const rifeofx::CachedFrame& frameB = *frames[static_cast<size_t>(kFramesBefore) + 1];
  const float amount = timing.timestep;
  if (getDebugParam(data, time)) {
    std::ostringstream stream;
    stream << "render outputTime=" << time
           << " sourceFrame=" << timing.sourceFrame
           << " timestep=" << amount;
    debugLog(stream.str());
  }
  const int x1 = std::max({renderWindow.x1, outputBounds.x1, frameA.bounds.x1,
                           frameB.bounds.x1});
  const int y1 = std::max({renderWindow.y1, outputBounds.y1, frameA.bounds.y1,
                           frameB.bounds.y1});
  const int x2 = std::min({renderWindow.x2, outputBounds.x2, frameA.bounds.x2,
                           frameB.bounds.x2});
  const int y2 = std::min({renderWindow.y2, outputBounds.y2, frameA.bounds.y2,
                           frameB.bounds.y2});
  const int frameAWidth = frameA.bounds.x2 - frameA.bounds.x1;
  std::vector<float> outputRGBA;
  if (amount <= 0.0f) {
    outputRGBA = frameA.rgba;
  } else if (amount >= 1.0f) {
    outputRGBA = frameB.rgba;
  } else {
#if RIFE_ENABLE_INFERENCE
    const std::string modelId = getSelectedModelId(data, time);
    status = data->rifeEngine->loadModel(modelId);
    if (status != kOfxStatOK) {
      gImageEffectSuite->clipReleaseImage(outputImage);
      return status;
    }
    rifeofx::InferenceDiagnostics diagnostics;
    status = data->rifeEngine->interpolate(frameA, frameB, amount, outputRGBA,
                                            &diagnostics);
    if (status != kOfxStatOK) {
      gImageEffectSuite->clipReleaseImage(outputImage);
      return status;
    }
    if (getDebugParam(data, time)) {
      std::ostringstream stream;
      stream << "model=" << diagnostics.modelId
             << " backend=" << diagnostics.backend
             << " gpu=" << diagnostics.gpuId
             << " input=" << diagnostics.inputWidth << "x" << diagnostics.inputHeight
             << " padded=" << diagnostics.paddedWidth << "x" << diagnostics.paddedHeight
             << " inferenceMs=" << diagnostics.inferenceMilliseconds;
      debugLog(stream.str());
    }
#else
    // Safe host-integration build: validate temporal access and use a CPU
    // blend while RIFE/Vulkan remains isolated in RifeSmokeTest.
    outputRGBA.resize(frameA.rgba.size());
    for (size_t index = 0; index < outputRGBA.size(); ++index) {
      outputRGBA[index] = frameA.rgba[index] * (1.0f - amount) +
                          frameB.rgba[index] * amount;
    }
#endif
  }

  if (x1 < x2 && y1 < y2) {
    constexpr int channels = 4;
    for (int y = y1; y < y2; ++y) {
      if (gImageEffectSuite->abort(effect)) {
        break;
      }
      auto* outputRow = static_cast<float*>(outputData) +
                        ((y - outputBounds.y1) * outputRowBytes / sizeof(float)) +
                        (x1 - outputBounds.x1) * channels;
      const float* sourceRow = outputRGBA.data() +
                               (y - frameA.bounds.y1) * frameAWidth * channels +
                               (x1 - frameA.bounds.x1) * channels;
      for (int x = x1; x < x2; ++x) {
        for (int channel = 0; channel < channels; ++channel) {
          outputRow[channel] = sourceRow[channel];
        }
        outputRow += channels;
        sourceRow += channels;
      }
    }
  }

  gImageEffectSuite->clipReleaseImage(outputImage);
  return kOfxStatOK;
}

OfxStatus pluginMain(const char* action, const void* handle,
                     OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
  if (!gHost && std::strcmp(action, kOfxActionLoad) != 0) {
    return kOfxStatErrMissingHostFeature;
  }

  if (std::strcmp(action, kOfxActionDescribe) == 0) {
    return describe(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)));
  }
  if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
    return describeInContext(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)), inArgs);
  }
  if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
    return createInstance(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)));
  }
  if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
    return destroyInstance(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)));
  }
  if (std::strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
    return isIdentity(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)),
                      inArgs, outArgs);
  }
  if (std::strcmp(action, kOfxImageEffectActionGetFramesNeeded) == 0) {
    return getFramesNeeded(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)),
                           inArgs, outArgs);
  }
  if (std::strcmp(action, kOfxImageEffectActionRender) == 0) {
    return render(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)), inArgs);
  }
  return kOfxStatReplyDefault;
}

void setHost(OfxHost* host) {
  gHost = host;
  if (host) {
    fetchSuites(host);
  }
}

OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    1,
    kPluginIdentifier,
    kPluginMajor,
    kPluginMinor,
    setHost,
    pluginMain,
};

}  // namespace

extern "C" __declspec(dllexport) int OfxGetNumberOfPlugins(void) {
  return 1;
}

extern "C" __declspec(dllexport) OfxPlugin* OfxGetPlugin(int nth) {
  return nth == 0 ? &gPlugin : nullptr;
}
