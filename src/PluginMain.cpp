#include <ofxCore.h>
#include <ofxImageEffect.h>
#include <ofxGPURender.h>
#include <ofxParam.h>

#if RIFE_ENABLE_INFERENCE
#include "RifeEngine.h"
#endif
#include "DebugLog.h"
#include "ModelRegistry.h"
#include "TemporalFrameProvider.h"
#include "TemporalMapping.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <cmath>
#include <iomanip>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr char kPluginIdentifier[] = "com.rifeofx.RifeFrameInterpolator";
constexpr int kPluginMajor = 0;
constexpr int kPluginMinor = 1;
constexpr char kEnabledParam[] = "enabled";
constexpr char kDetectedFrameRateParam[] = "detectedFrameRate";
constexpr char kSourceFrameRateParam[] = "sourceFrameRate";
constexpr char kUseTimelineFrameRateParam[] = "useTimelineFrameRate";
constexpr char kTargetFrameRateParam[] = "targetFrameRate";
constexpr char kSourceTimeBaseParam[] = "sourceTimeBase";
constexpr char kModeParam[] = "mode";
constexpr char kQualityParam[] = "quality";
constexpr char kDebugParam[] = "debug";
constexpr char kModelParam[] = "model";
constexpr char kGpuDeviceParam[] = "gpuDevice";
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

bool environmentFlag(const wchar_t* name) {
  const wchar_t* value = _wgetenv(name);
  return value && *value && value[0] != L'0';
}

// Forces the detailed temporal trace on before the user has a chance to toggle
// the Debug parameter, which matters when diagnosing describe/createInstance.
bool forceDebug() {
  static const bool forced = environmentFlag(L"RIFEOFX_DEBUG");
  return forced;
}

// Opt-in probe: additionally advertise kOfxImageEffectContextRetimer so the log
// shows whether the host ever describes or instantiates this plugin as a
// retimer. Off by default so the primary filter test is never disturbed.
bool probeRetimerContext() {
  static const bool probe = environmentFlag(L"RIFEOFX_PROBE_RETIMER");
  return probe;
}

// Everything the host told us about the temporal layout of the clips. All of it
// is logged verbatim: the point of this build is to find out what Resolve
// actually exposes, not to assume it.
struct ClipTimingInfo {
  double sourceFrameRate = 0.0;
  bool sourceMappedRateAvailable = false;
  double sourceMappedFrameRate = 0.0;
  bool sourceUnmappedRateAvailable = false;
  double sourceUnmappedFrameRate = 0.0;

  double outputFrameRate = 0.0;
  bool outputMappedRateAvailable = false;
  double outputMappedFrameRate = 0.0;
  bool projectRateAvailable = false;
  double projectFrameRate = 0.0;

  bool sourceRangeAvailable = false;
  OfxTime sourceFirstFrame = 0.0;
  OfxTime sourceLastFrame = 0.0;
  bool sourceUnmappedRangeAvailable = false;
  OfxTime sourceUnmappedFirstFrame = 0.0;
  OfxTime sourceUnmappedLastFrame = 0.0;

  bool outputRangeAvailable = false;
  OfxTime outputFirstFrame = 0.0;
  OfxTime outputLastFrame = 0.0;

  bool sourceContinuousSamples = false;
  bool sourceConnected = false;
};

struct InstanceData {
  OfxImageEffectHandle effect = nullptr;
  OfxImageClipHandle sourceClip = nullptr;
  OfxImageClipHandle outputClip = nullptr;
  OfxParamHandle enabledParam = nullptr;
  OfxParamHandle detectedFrameRateParam = nullptr;
  OfxParamHandle sourceFrameRateParam = nullptr;
  OfxParamHandle useTimelineFrameRateParam = nullptr;
  OfxParamHandle targetFrameRateParam = nullptr;
  OfxParamHandle sourceTimeBaseParam = nullptr;
  OfxParamHandle modeParam = nullptr;
  OfxParamHandle qualityParam = nullptr;
  OfxParamHandle debugParam = nullptr;
  OfxParamHandle modelParam = nullptr;
  OfxParamHandle gpuDeviceParam = nullptr;
  // Host-managed pseudo parameter, only present in the retimer context.
  OfxParamHandle sourceTimeParam = nullptr;
  bool retimerContext = false;

  std::string contextName;
  std::unique_ptr<rifeofx::TemporalFrameProvider> temporalProvider;
  ClipTimingInfo timing;
  // Previous RetimerProbe sample, used to report how SourceTime advances
  // relative to the render time.
  bool retimerProbePrimed = false;
  OfxTime retimerProbePreviousOutputTime = 0.0;
  double retimerProbePreviousSourceTime = 0.0;
  // One-shot diagnostics, run on the first render that has Debug enabled.
  bool firstRenderLogged = false;
  bool renderTimeProbeDone = false;
  // TemporalFrameProvider owns mutable cache state and returns pointers into
  // it. Serialize render calls for this instance while preserving parallelism
  // between independent effect instances.
  std::mutex renderMutex;
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
    rifeofx::debugLog("RIFEOFX_MODELS_ROOT is not a directory; using bundle models root");
  }
  return bundleModelsRoot;
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

// Everything the retimer experiment produces carries this tag so a single
// DebugView filter isolates it from the temporal trace.
void retimerProbeLog(const std::string& message) {
  const std::string tagged = "[RifeOFX][RetimerProbe] " + message;
  rifeofx::debugLogBlock(tagged);
  rifeofx::appendTemporalLog(tagged);
}

void retimerProbeBlock(const std::string& body) {
  const std::string tagged = "[RifeOFX][RetimerProbe]\n" + body;
  rifeofx::debugLogBlock(tagged);
  rifeofx::appendTemporalLog(tagged);
}

void logHostInt(const char* property, const char* label) {
  int value = 0;
  if (gPropertySuite->propGetInt(gHost->host, property, 0, &value) == kOfxStatOK) {
    std::ostringstream stream;
    stream << "host " << label << "=" << value;
    rifeofx::debugLog(stream.str());
  }
}

bool getBoolParam(const InstanceData* data, OfxTime time) {
  int enabled = 1;
  if (!data || !data->enabledParam ||
      gParameterSuite->paramGetValueAtTime(data->enabledParam, time, &enabled) != kOfxStatOK) {
    return true;
  }
  return enabled != 0;
}

bool getDebugParam(const InstanceData* data, OfxTime time) {
  if (forceDebug()) {
    return true;
  }
  int debug = 0;
  if (!data || !data->debugParam ||
      gParameterSuite->paramGetValueAtTime(data->debugParam, time, &debug) != kOfxStatOK) {
    return false;
  }
  return debug != 0;
}

bool getUseTimelineFrameRate(const InstanceData* data, OfxTime time) {
  int useTimelineFrameRate = 1;
  if (!data || !data->useTimelineFrameRateParam ||
      gParameterSuite->paramGetValueAtTime(data->useTimelineFrameRateParam, time,
                                            &useTimelineFrameRate) != kOfxStatOK) {
    return true;
  }
  return useTimelineFrameRate != 0;
}

double getTargetFrameRate(const InstanceData* data, OfxTime time) {
  const double detectedTimelineFrameRate =
      data && data->timing.outputFrameRate > 0.0 ? data->timing.outputFrameRate : 60.0;
  if (getUseTimelineFrameRate(data, time)) {
    return detectedTimelineFrameRate;
  }

  double targetFrameRate = detectedTimelineFrameRate;
  if (!data || !data->targetFrameRateParam ||
      gParameterSuite->paramGetValueAtTime(data->targetFrameRateParam, time,
                                            &targetFrameRate) != kOfxStatOK) {
    return targetFrameRate;
  }
  return targetFrameRate > 0.0 ? targetFrameRate : 60.0;
}

double getSourceFrameRate(const InstanceData* data, OfxTime time) {
  double sourceFrameRate =
      data && data->timing.sourceFrameRate > 0.0 ? data->timing.sourceFrameRate : 60.0;
  if (!data || !data->sourceFrameRateParam ||
      gParameterSuite->paramGetValueAtTime(data->sourceFrameRateParam, time,
                                            &sourceFrameRate) != kOfxStatOK) {
    return sourceFrameRate;
  }
  return sourceFrameRate > 0.0 ? sourceFrameRate : 60.0;
}

int getChoiceParam(OfxParamHandle parameter, OfxTime time, int fallback) {
  int value = fallback;
  if (!parameter ||
      gParameterSuite->paramGetValueAtTime(parameter, time, &value) != kOfxStatOK) {
    return fallback;
  }
  return value;
}

rifeofx::SourceTimeBase getSourceTimeBase(const InstanceData* data, OfxTime time) {
  const int selected =
      getChoiceParam(data ? data->sourceTimeBaseParam : nullptr, time, 0);
  return selected == 1 ? rifeofx::SourceTimeBase::kTimelineFrames
                       : rifeofx::SourceTimeBase::kSourceFrames;
}

void setParameterEnabled(OfxParamHandle parameter, bool enabled) {
  if (!parameter) {
    return;
  }
  OfxPropertySetHandle properties = nullptr;
  if (gParameterSuite->paramGetPropertySet(parameter, &properties) == kOfxStatOK &&
      properties) {
    gPropertySuite->propSetInt(properties, kOfxParamPropEnabled, 0,
                               enabled ? 1 : 0);
  }
}

void updateTargetFrameRateEnabled(InstanceData* data, OfxTime time) {
  if (!data) {
    return;
  }
  setParameterEnabled(data->targetFrameRateParam,
                      !getUseTimelineFrameRate(data, time));
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

bool readDoubleProperty(OfxPropertySetHandle properties, const char* property,
                        double* value) {
  return properties && property && value &&
         gPropertySuite->propGetDouble(properties, property, 0, value) == kOfxStatOK;
}

bool readPositiveDoubleProperty(OfxPropertySetHandle properties,
                                const char* property, double* value) {
  return readDoubleProperty(properties, property, value) && *value > 0.0 &&
         std::isfinite(*value);
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

bool readFrameRange(OfxPropertySetHandle properties, const char* property,
                    OfxTime* first, OfxTime* last) {
  double range[2] = {};
  if (!properties ||
      gPropertySuite->propGetDoubleN(properties, property, 2, range) != kOfxStatOK ||
      !isUsableFrameRange(range)) {
    return false;
  }
  *first = range[0];
  *last = range[1];
  return true;
}

std::string formatOptionalDouble(bool available, double value) {
  if (!available) {
    return "unavailable";
  }
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(6) << value;
  return stream.str();
}

std::string formatOptionalRange(bool available, OfxTime first, OfxTime last) {
  if (!available) {
    return "unavailable";
  }
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3) << first << ".." << last;
  return stream.str();
}

// Reads every timing property the host exposes for the Source and Output clips.
// Called on instance creation and whenever a clip changes so a reconnect cannot
// leave a stale cadence behind.
void refreshClipTiming(InstanceData* data) {
  if (!data || !data->sourceClip || !data->outputClip || !data->effect) {
    return;
  }

  ClipTimingInfo timing;
  OfxPropertySetHandle sourceProperties = nullptr;
  OfxPropertySetHandle outputProperties = nullptr;
  OfxPropertySetHandle effectProperties = nullptr;
  const OfxStatus sourceStatus =
      gImageEffectSuite->clipGetPropertySet(data->sourceClip, &sourceProperties);
  const OfxStatus outputStatus =
      gImageEffectSuite->clipGetPropertySet(data->outputClip, &outputProperties);
  const OfxStatus effectStatus =
      gImageEffectSuite->getPropertySet(data->effect, &effectProperties);

  if (sourceStatus == kOfxStatOK) {
    timing.sourceMappedRateAvailable = readPositiveDoubleProperty(
        sourceProperties, kOfxImageEffectPropFrameRate, &timing.sourceMappedFrameRate);
    timing.sourceUnmappedRateAvailable = readPositiveDoubleProperty(
        sourceProperties, kOfxImageEffectPropUnmappedFrameRate,
        &timing.sourceUnmappedFrameRate);
    timing.sourceRangeAvailable =
        readFrameRange(sourceProperties, kOfxImageEffectPropFrameRange,
                       &timing.sourceFirstFrame, &timing.sourceLastFrame);
    timing.sourceUnmappedRangeAvailable = readFrameRange(
        sourceProperties, kOfxImageEffectPropUnmappedFrameRange,
        &timing.sourceUnmappedFirstFrame, &timing.sourceUnmappedLastFrame);

    int continuous = 0;
    if (gPropertySuite->propGetInt(sourceProperties,
                                  kOfxImageClipPropContinuousSamples, 0,
                                  &continuous) == kOfxStatOK) {
      timing.sourceContinuousSamples = continuous != 0;
    }
    int connected = 0;
    if (gPropertySuite->propGetInt(sourceProperties, kOfxImageClipPropConnected, 0,
                                  &connected) == kOfxStatOK) {
      timing.sourceConnected = connected != 0;
    }
  }

  if (outputStatus == kOfxStatOK) {
    timing.outputMappedRateAvailable = readPositiveDoubleProperty(
        outputProperties, kOfxImageEffectPropFrameRate, &timing.outputMappedFrameRate);
    timing.outputRangeAvailable =
        readFrameRange(outputProperties, kOfxImageEffectPropFrameRange,
                       &timing.outputFirstFrame, &timing.outputLastFrame);
  }

  if (effectStatus == kOfxStatOK) {
    // On an effect instance kOfxImageEffectPropFrameRate is the project rate.
    timing.projectRateAvailable = readPositiveDoubleProperty(
        effectProperties, kOfxImageEffectPropFrameRate, &timing.projectFrameRate);
  }

  // Resolve may map the source clip onto the timeline rate when exposing
  // kOfxImageEffectPropFrameRate. The unmapped property is the one that keeps
  // the original media cadence, so it wins when both are present.
  if (timing.sourceUnmappedRateAvailable) {
    timing.sourceFrameRate = timing.sourceUnmappedFrameRate;
  } else if (timing.sourceMappedRateAvailable) {
    timing.sourceFrameRate = timing.sourceMappedFrameRate;
  }

  if (timing.projectRateAvailable) {
    timing.outputFrameRate = timing.projectFrameRate;
  } else if (timing.outputMappedRateAvailable) {
    timing.outputFrameRate = timing.outputMappedFrameRate;
  }

  data->timing = timing;
}

void logClipTiming(const InstanceData* data) {
  if (!data) {
    return;
  }
  const ClipTimingInfo& timing = data->timing;
  std::ostringstream stream;
  stream << "clip timing"
         << " sourceConnected=" << (timing.sourceConnected ? 1 : 0)
         << " sourceFrameRate(mapped)="
         << formatOptionalDouble(timing.sourceMappedRateAvailable,
                                 timing.sourceMappedFrameRate)
         << " sourceFrameRate(unmapped)="
         << formatOptionalDouble(timing.sourceUnmappedRateAvailable,
                                 timing.sourceUnmappedFrameRate)
         << " outputFrameRate(mapped)="
         << formatOptionalDouble(timing.outputMappedRateAvailable,
                                 timing.outputMappedFrameRate)
         << " projectFrameRate="
         << formatOptionalDouble(timing.projectRateAvailable, timing.projectFrameRate)
         << " sourceFrameRange="
         << formatOptionalRange(timing.sourceRangeAvailable, timing.sourceFirstFrame,
                                timing.sourceLastFrame)
         << " sourceUnmappedFrameRange="
         << formatOptionalRange(timing.sourceUnmappedRangeAvailable,
                                timing.sourceUnmappedFirstFrame,
                                timing.sourceUnmappedLastFrame)
         << " outputFrameRange="
         << formatOptionalRange(timing.outputRangeAvailable, timing.outputFirstFrame,
                                timing.outputLastFrame)
         << " continuousSamples=" << (timing.sourceContinuousSamples ? 1 : 0);
  rifeofx::debugLog(stream.str());
  rifeofx::appendTemporalLog(stream.str());
}

struct TemporalAnchor {
  // OfxTime that corresponds to source position 0, i.e. where the clip starts
  // on the axis the host uses for kOfxPropTime.
  OfxTime origin = 0.0;
  bool clampToSourceRange = false;
  bool resolved = false;
  const char* reason = "unresolved";
};

// Resolve reports render times on a composition-global axis (observed:
// outputTime=6816) while the Source clip range can be media-local (0..1999).
// A range from a different axis must never be used to locate the clip or to
// clamp a request: doing so pins every render to the same frame.
//
// The test is the render time itself, not a derived value: a range is only
// usable when kOfxPropTime actually falls inside it.
TemporalAnchor resolveTemporalAnchor(const ClipTimingInfo& timing,
                                     OfxTime outputTime) {
  constexpr double kTolerance = 0.5;
  TemporalAnchor anchor;

  if (timing.sourceRangeAvailable &&
      outputTime >= timing.sourceFirstFrame - kTolerance &&
      outputTime <= timing.sourceLastFrame + kTolerance) {
    anchor.origin = timing.sourceFirstFrame;
    anchor.clampToSourceRange = true;
    anchor.resolved = true;
    anchor.reason = "sourceFrameRange";
    return anchor;
  }

  if (timing.outputRangeAvailable &&
      outputTime >= timing.outputFirstFrame - kTolerance &&
      outputTime <= timing.outputLastFrame + kTolerance) {
    // The clip is located on the output axis, but the source range belongs to
    // another axis, so it cannot bound the request.
    anchor.origin = timing.outputFirstFrame;
    anchor.clampToSourceRange = false;
    anchor.resolved = true;
    anchor.reason = "outputFrameRange";
    return anchor;
  }

  // Nothing the host reported locates this clip on the render axis. Anchoring
  // at the render time degrades to a passthrough instead of confidently asking
  // for a source frame hundreds of frames away from the playhead.
  anchor.origin = outputTime;
  anchor.clampToSourceRange = false;
  anchor.resolved = false;
  anchor.reason = "unresolved (render time outside every reported range)";
  return anchor;
}

// Output time -> position in the original media -> the two source frames that
// bracket it. This is the single place the mapping is decided; render and
// getFramesNeeded both go through it so they can never disagree.
rifeofx::TemporalMapping buildTemporalMapping(const InstanceData* data,
                                              OfxTime outputTime) {
  rifeofx::TemporalMappingInputs inputs;
  inputs.outputTime = outputTime;
  inputs.sourceFrameRate = getSourceFrameRate(data, outputTime);
  inputs.outputFrameRate = getTargetFrameRate(data, outputTime);
  inputs.timeBase = getSourceTimeBase(data, outputTime);
  if (data) {
    const TemporalAnchor anchor = resolveTemporalAnchor(data->timing, outputTime);
    inputs.timelineOrigin = anchor.origin;
    inputs.anchorResolved = anchor.resolved;
    inputs.anchorReason = anchor.reason;
    inputs.clampToSourceRange = anchor.clampToSourceRange;
    inputs.sourceFirstFrame = data->timing.sourceFirstFrame;
    inputs.sourceLastFrame = data->timing.sourceLastFrame;
  }

  // In the OpenFX retimer context the host owns the mapping and publishes it
  // through the mandated SourceTime parameter, expressed in source frames.
  if (data && data->retimerContext && data->sourceTimeParam) {
    double hostSourceTime = 0.0;
    if (gParameterSuite->paramGetValueAtTime(data->sourceTimeParam, outputTime,
                                             &hostSourceTime) == kOfxStatOK) {
      inputs.hostSourcePositionValid = true;
      inputs.hostSourcePosition = hostSourceTime;
    }
  }

  return rifeofx::computeTemporalMapping(inputs);
}

struct TemporalInputs {
  const rifeofx::CachedFrame* frameA = nullptr;
  const rifeofx::CachedFrame* frameB = nullptr;
  std::uint64_t signatureA = 0;
  std::uint64_t signatureB = 0;
  bool signaturesComputed = false;
  bool identicalImages = false;
  // Set when the trailing neighbour could not be fetched or does not have the
  // same geometry. Render then holds frame A rather than blending mismatched
  // buffers.
  bool neighbourUnavailable = false;
};

bool sameBounds(const OfxRectI& left, const OfxRectI& right) {
  return left.x1 == right.x1 && left.y1 == right.y1 && left.x2 == right.x2 &&
         left.y2 == right.y2;
}

// Explicit temporal read: both source images are requested from the host by the
// times computed above, never by args.time.
OfxStatus fetchTemporalInputs(InstanceData* data,
                              const rifeofx::TemporalMapping& mapping, bool debug,
                              TemporalInputs* inputs) {
  if (!data || !data->temporalProvider || !inputs) {
    return kOfxStatErrBadHandle;
  }
  data->temporalProvider->setDebug(debug);
  data->temporalProvider->beginOutputFrame();

  if (debug) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "fetchImage A requestedTime=" << mapping.sourceTimeA;
    rifeofx::debugLog(stream.str());
  }
  OfxStatus status =
      data->temporalProvider->getFrame(mapping.sourceTimeA, &inputs->frameA);
  if (status != kOfxStatOK || !inputs->frameA) {
    return status == kOfxStatOK ? kOfxStatFailed : status;
  }

  inputs->frameB = inputs->frameA;
  if (!mapping.singleImage()) {
    if (debug) {
      std::ostringstream stream;
      stream << std::fixed << std::setprecision(6)
             << "fetchImage B requestedTime=" << mapping.sourceTimeB;
      rifeofx::debugLog(stream.str());
    }
    const rifeofx::CachedFrame* frameB = nullptr;
    status = data->temporalProvider->getFrame(mapping.sourceTimeB, &frameB);
    if (status != kOfxStatOK || !frameB) {
      // Explicit edge policy: a missing trailing neighbour holds frame A. The
      // status is logged so a systematic failure is visible instead of looking
      // like a clean passthrough.
      std::ostringstream stream;
      stream << std::fixed << std::setprecision(6)
             << "fetchImage B unavailable requestedTime=" << mapping.sourceTimeB
             << " status=" << status << "; holding frame A";
      rifeofx::debugLog(stream.str());
      rifeofx::appendTemporalLog(stream.str());
      inputs->neighbourUnavailable = true;
    } else if (!sameBounds(frameB->bounds, inputs->frameA->bounds)) {
      std::ostringstream stream;
      stream << "fetchImage B geometry mismatch A=["
             << inputs->frameA->bounds.x1 << "," << inputs->frameA->bounds.y1 << ","
             << inputs->frameA->bounds.x2 << "," << inputs->frameA->bounds.y2
             << "] B=[" << frameB->bounds.x1 << "," << frameB->bounds.y1 << ","
             << frameB->bounds.x2 << "," << frameB->bounds.y2
             << "]; holding frame A";
      rifeofx::debugLog(stream.str());
      rifeofx::appendTemporalLog(stream.str());
      inputs->neighbourUnavailable = true;
    } else {
      inputs->frameB = frameB;
    }
  }

  if (debug) {
    inputs->signatureA = rifeofx::sampledSignature(inputs->frameA->rgba.data(),
                                                    inputs->frameA->rgba.size());
    inputs->signatureB = inputs->frameB == inputs->frameA
                             ? inputs->signatureA
                             : rifeofx::sampledSignature(inputs->frameB->rgba.data(),
                                                         inputs->frameB->rgba.size());
    inputs->signaturesComputed = true;
    inputs->identicalImages = inputs->signatureA == inputs->signatureB;
  }
  return kOfxStatOK;
}

// Asks the host for the source image at the raw render time, once per instance.
// This is the measurement that decides which axis the input clip is addressed
// on: if kOfxPropTime is a valid source time then the clip lives on the render
// axis and a media-local frame range must not be used to locate it.
void probeRenderTimeAddressing(InstanceData* data, OfxTime renderTime,
                               const rifeofx::TemporalMapping& mapping,
                               const TemporalInputs& inputs) {
  const rifeofx::CachedFrame* probeFrame = nullptr;
  const OfxStatus status =
      data->temporalProvider->getFrame(renderTime, &probeFrame);

  std::ostringstream stream;
  stream << std::fixed << std::setprecision(6)
         << "renderTimeProbe requestedTime=" << renderTime
         << " status=" << status;
  if (status == kOfxStatOK && probeFrame) {
    const std::uint64_t signature = rifeofx::sampledSignature(
        probeFrame->rgba.data(), probeFrame->rgba.size());
    stream << " addressable=1 signature=" << rifeofx::toHex(signature)
           << " matchesMappedFrameA="
           << (inputs.signaturesComputed && signature == inputs.signatureA ? 1 : 0)
           << " mappedTimeA=" << mapping.sourceTimeA;
  } else {
    stream << " addressable=0";
  }
  rifeofx::debugLog(stream.str());
  rifeofx::appendTemporalLog(stream.str());
}

// Per output frame, only while RIFEOFX_PROBE_RETIMER is set. Reports the value
// the host publishes in the mandated SourceTime parameter and how it advances
// against the render time, which is what distinguishes a source position driven
// by the original media cadence from one that merely follows the timeline.
void logRetimerProbe(InstanceData* data, OfxTime outputTime) {
  double sourceTime = 0.0;
  OfxStatus sourceTimeStatus = kOfxStatErrUnknown;
  if (data->sourceTimeParam) {
    sourceTimeStatus = gParameterSuite->paramGetValueAtTime(
        data->sourceTimeParam, outputTime, &sourceTime);
  }

  const double sourceClipFps = data->timing.sourceUnmappedRateAvailable
                                   ? data->timing.sourceUnmappedFrameRate
                                   : data->timing.sourceMappedFrameRate;
  const double hostFps = data->timing.projectRateAvailable
                             ? data->timing.projectFrameRate
                             : data->timing.outputFrameRate;
  const double manualSourceFps = getSourceFrameRate(data, outputTime);

  std::ostringstream stream;
  stream << std::fixed;
  stream << "context=" << (data->contextName.empty() ? "unknown" : data->contextName)
         << "\n";
  stream << "outputTime=" << std::setprecision(3) << outputTime << "\n";
  if (sourceTimeStatus == kOfxStatOK) {
    stream << "sourceTime=" << std::setprecision(6) << sourceTime << "\n";
  } else {
    stream << "sourceTime=absent paramPresent="
           << (data->sourceTimeParam ? 1 : 0)
           << " getValueStatus=" << sourceTimeStatus << "\n";
  }

  if (sourceTimeStatus == kOfxStatOK && data->retimerProbePrimed) {
    const double outputDelta = outputTime - data->retimerProbePreviousOutputTime;
    const double sourceDelta = sourceTime - data->retimerProbePreviousSourceTime;
    stream << "outputTimeDelta=" << std::setprecision(6) << outputDelta << "\n";
    stream << "sourceTimeDelta=" << std::setprecision(6) << sourceDelta << "\n";
    if (std::abs(outputDelta) > 1e-9) {
      // 1.0 means SourceTime just follows the timeline. sourceFPS/outputFPS
      // (0.8333 for 50 into 60) means it follows the original media cadence.
      stream << "sourcePerOutput=" << std::setprecision(6)
             << (sourceDelta / outputDelta) << "\n";
    }
  } else {
    stream << "sourceTimeDelta=n/a (first sample or no SourceTime)\n";
  }

  stream << "sourceClipFPS=" << std::setprecision(3) << sourceClipFps << "\n";
  stream << "hostFPS=" << std::setprecision(3) << hostFps << "\n";
  stream << "manualSourceFPS=" << std::setprecision(3) << manualSourceFps << "\n";
  if (hostFps > 0.0) {
    stream << "expectedSourcePerOutputIfMediaRate=" << std::setprecision(6)
           << (manualSourceFps / hostFps) << "\n";
  }
  stream << "sourceTimeParamPresent=" << (data->sourceTimeParam ? 1 : 0)
         << " retimerContext=" << (data->retimerContext ? 1 : 0);
  retimerProbeBlock(stream.str());

  if (sourceTimeStatus == kOfxStatOK) {
    data->retimerProbePrimed = true;
    data->retimerProbePreviousOutputTime = outputTime;
    data->retimerProbePreviousSourceTime = sourceTime;
  }
}

// Once per instance: label a run of consecutive render times by the first time
// that produced each image. A host that conformed the media to the timeline
// cadence repeats one image per cycle, so the pattern shows both that fact and
// its phase, which is what decides whether the original frames are reachable at
// all and at which times.
void probeInputCadence(InstanceData* data, OfxTime renderTime) {
  constexpr int kProbeLength = 13;
  std::vector<std::uint64_t> signatures;
  signatures.reserve(kProbeLength);

  std::ostringstream pattern;
  int distinct = 0;
  for (int offset = 0; offset < kProbeLength; ++offset) {
    std::uint64_t signature = 0;
    if (data->temporalProvider->signatureAt(renderTime + offset, &signature) !=
        kOfxStatOK) {
      pattern << '?';
      signatures.push_back(0);
      continue;
    }
    std::size_t first = 0;
    while (first < signatures.size() && signatures[first] != signature) {
      ++first;
    }
    if (first == signatures.size()) {
      ++distinct;
    }
    signatures.push_back(signature);
    pattern << static_cast<char>('A' + static_cast<int>(first % 26));
  }

  std::ostringstream stream;
  stream << std::fixed << std::setprecision(3)
         << "cadenceProbe from=" << renderTime << " length=" << kProbeLength
         << " pattern=" << pattern.str() << " distinct=" << distinct;
  rifeofx::debugLog(stream.str());
  rifeofx::appendTemporalLog(stream.str());
}

// The only entry point into the inference engine. Kept separate from the OFX
// plumbing so additional models or interpolation strategies plug in here.
OfxStatus runRifeInterpolation(InstanceData* data, OfxTime time,
                               const rifeofx::CachedFrame& frameA,
                               const rifeofx::CachedFrame& frameB, float timestep,
                               bool debug, std::vector<float>& outputRGBA) {
#if RIFE_ENABLE_INFERENCE
  const std::string modelId = getSelectedModelId(data, time);
  OfxStatus status = data->rifeEngine->loadModel(modelId);
  if (status != kOfxStatOK) {
    return status;
  }
  rifeofx::InferenceDiagnostics diagnostics;
  status = data->rifeEngine->interpolate(frameA, frameB, timestep, outputRGBA,
                                          &diagnostics);
  if (status != kOfxStatOK) {
    return status;
  }
  if (debug) {
    std::ostringstream stream;
    stream << "model=" << diagnostics.modelId
           << " backend=" << diagnostics.backend
           << " gpu=" << diagnostics.gpuId
           << " input=" << diagnostics.inputWidth << "x" << diagnostics.inputHeight
           << " padded=" << diagnostics.paddedWidth << "x" << diagnostics.paddedHeight
           << " timestep=" << timestep
           << " inferenceMs=" << diagnostics.inferenceMilliseconds;
    rifeofx::debugLog(stream.str());
  }
  return kOfxStatOK;
#else
  // Safe host-integration build: validate temporal access with a CPU blend
  // while RIFE/Vulkan stays isolated in RifeSmokeTest.
  (void)data;
  (void)time;
  (void)debug;
  outputRGBA.resize(frameA.rgba.size());
  const size_t count = std::min(outputRGBA.size(), frameB.rgba.size());
  for (size_t index = 0; index < count; ++index) {
    outputRGBA[index] = frameA.rgba[index] * (1.0f - timestep) +
                        frameB.rgba[index] * timestep;
  }
  return kOfxStatOK;
#endif
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
  if (probeRetimerContext()) {
    // Filter stays at index 0 so a host that supports both keeps using it.
    const OfxStatus retimerContextStatus = gPropertySuite->propSetString(
        effectProps, kOfxImageEffectPropSupportedContexts, 1,
        kOfxImageEffectContextRetimer);
    retimerProbeLog(
        "advertising kOfxImageEffectContextRetimer at index 1, setStatus=" +
        std::to_string(retimerContextStatus));

    // Read the property back. If the host rejected the retimer entry it either
    // reports a smaller dimension or a different string, and that is the answer
    // to whether the context is accepted at all.
    int declaredContexts = 0;
    const OfxStatus dimensionStatus = gPropertySuite->propGetDimension(
        effectProps, kOfxImageEffectPropSupportedContexts, &declaredContexts);
    std::ostringstream contexts;
    contexts << "supportedContexts readBackStatus=" << dimensionStatus
             << " dimension=" << declaredContexts;
    for (int index = 0; index < declaredContexts; ++index) {
      char* value = nullptr;
      const OfxStatus valueStatus = gPropertySuite->propGetString(
          effectProps, kOfxImageEffectPropSupportedContexts, index, &value);
      contexts << " [" << index << "]="
               << (valueStatus == kOfxStatOK && value ? value : "<unreadable>");
    }
    retimerProbeLog(contexts.str());
  }
  setString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0,
           kOfxBitDepthFloat);
  setInt(effectProps, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
  setInt(effectProps, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
  setInt(effectProps, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
  setInt(effectProps, kOfxImageEffectPropSupportsTiles, 0, 0);

  // Random temporal access on the input clip. Without this the host never calls
  // kOfxImageEffectActionGetFramesNeeded and clipGetImage at another time is
  // not guaranteed to work.
  const OfxStatus temporalStatus = gPropertySuite->propSetInt(
      effectProps, kOfxImageEffectPropTemporalClipAccess, 0, 1);
  int temporalReadBack = -1;
  const OfxStatus temporalReadStatus = gPropertySuite->propGetInt(
      effectProps, kOfxImageEffectPropTemporalClipAccess, 0, &temporalReadBack);
  {
    std::ostringstream stream;
    stream << "plugin temporalClipAccess setStatus=" << temporalStatus
           << " readBackStatus=" << temporalReadStatus
           << " value=" << temporalReadBack;
    rifeofx::debugLog(stream.str());
  }

  // The target rate determines the effect's output cadence. Declaring these
  // dependencies makes Resolve ask for fresh clip preferences if the user
  // changes the cadence controls.
  setString(effectProps, kOfxImageEffectPropClipPreferencesSlaveParam, 0,
            kUseTimelineFrameRateParam);
  setString(effectProps, kOfxImageEffectPropClipPreferencesSlaveParam, 1,
            kTargetFrameRateParam);
  setString(effectProps, kOfxImageEffectPropClipPreferencesSlaveParam, 2,
            kSourceFrameRateParam);
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
    rifeofx::debugLog(std::string("host name=") + hostName);
  }
  if (gPropertySuite->propGetString(gHost->host, kOfxPropVersion, 0, &hostVersion) == kOfxStatOK &&
      hostVersion) {
    rifeofx::debugLog(std::string("host version=") + hostVersion);
  }
  {
    std::ostringstream stream;
    stream << "temporal log file=" << rifeofx::temporalLogPath();
    rifeofx::debugLog(stream.str());
  }

  return kOfxStatOK;
}

OfxStatus describeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  char* context = nullptr;
  if (gPropertySuite->propGetString(inArgs, kOfxImageEffectPropContext, 0, &context) != kOfxStatOK ||
      !context) {
    return kOfxStatReplyDefault;
  }

  // Logged unconditionally: this line is the empirical answer to "does this host
  // ever offer the retimer context to a third party plugin?".
  retimerProbeLog(std::string("describeInContext context=") + context);

  const bool filterContext =
      std::strcmp(context, kOfxImageEffectContextFilter) == 0;
  const bool retimerContext =
      std::strcmp(context, kOfxImageEffectContextRetimer) == 0;
  if (!filterContext && !retimerContext) {
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
  // Random temporal access is declared on the clip that will be read off the
  // current time, in addition to the plugin descriptor.
  const OfxStatus clipTemporalStatus = gPropertySuite->propSetInt(
      clipProps, kOfxImageEffectPropTemporalClipAccess, 0, 1);
  int clipTemporalReadBack = -1;
  const OfxStatus clipTemporalReadStatus = gPropertySuite->propGetInt(
      clipProps, kOfxImageEffectPropTemporalClipAccess, 0, &clipTemporalReadBack);
  {
    std::ostringstream stream;
    stream << "source clip temporalClipAccess setStatus=" << clipTemporalStatus
           << " readBackStatus=" << clipTemporalReadStatus
           << " value=" << clipTemporalReadBack;
    rifeofx::debugLog(stream.str());
  }

  OfxParamSetHandle paramSet = nullptr;
  status = gImageEffectSuite->getParamSet(effect, &paramSet);
  if (status != kOfxStatOK) {
    return status;
  }

  OfxPropertySetHandle paramProps = nullptr;
  if (retimerContext) {
    // Mandated by the retimer context. It carries no plugin-side UI: the host
    // writes the source position into it and the plugin only reads it.
    status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeDouble,
                                          kOfxImageEffectRetimerParamName,
                                          &paramProps);
    retimerProbeLog(std::string("paramDefine ") +
                    kOfxImageEffectRetimerParamName +
                    " status=" + std::to_string(status));
    if (status != kOfxStatOK) {
      return status;
    }
  }

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeBoolean,
                                        kEnabledParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Enabled");
  setString(paramProps, kOfxParamPropScriptName, 0, kEnabledParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Enable or disable the RIFE temporal render");
  setInt(paramProps, kOfxParamPropDefault, 0, 1);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeDouble,
                                        kDetectedFrameRateParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Detected Framerate");
  setString(paramProps, kOfxParamPropScriptName, 0, kDetectedFrameRateParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Detected input/source frame rate (read-only)");
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDefault, 0, 0.0);
  setInt(paramProps, kOfxParamPropEnabled, 0, 0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeDouble,
                                        kSourceFrameRateParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Source Framerate");
  setString(paramProps, kOfxParamPropScriptName, 0, kSourceFrameRateParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Original media framerate used to compute the source position. Set this "
            "manually when the host only exposes the conformed timeline rate.");
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDefault, 0, 60.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropMin, 0, 1.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropMax, 0, 1000.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDisplayMin, 0, 1.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDisplayMax, 0, 240.0);
  setInt(paramProps, kOfxParamPropAnimates, 0, 0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeBoolean,
                                        kUseTimelineFrameRateParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Use Timeline Framerate");
  setString(paramProps, kOfxParamPropScriptName, 0, kUseTimelineFrameRateParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Use the detected timeline framerate; disable this to edit Target Framerate");
  setInt(paramProps, kOfxParamPropDefault, 0, 1);
  setInt(paramProps, kOfxParamPropAnimates, 0, 0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeDouble,
                                        kTargetFrameRateParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Target Framerate");
  setString(paramProps, kOfxParamPropScriptName, 0, kTargetFrameRateParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "Output framerate used to convert the render time into seconds");
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDefault, 0, 60.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropMin, 0, 1.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropMax, 0, 1000.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDisplayMin, 0, 1.0);
  gPropertySuite->propSetDouble(paramProps, kOfxParamPropDisplayMax, 0, 240.0);
  setInt(paramProps, kOfxParamPropEnabled, 0, 0);
  setInt(paramProps, kOfxParamPropAnimates, 0, 0);

  status = gParameterSuite->paramDefine(paramSet, kOfxParamTypeChoice,
                                        kSourceTimeBaseParam, &paramProps);
  if (status != kOfxStatOK) {
    return status;
  }
  setString(paramProps, kOfxPropLabel, 0, "Source Time Base");
  setString(paramProps, kOfxParamPropScriptName, 0, kSourceTimeBaseParam);
  setString(paramProps, kOfxParamPropHint, 0,
            "How a source frame index is converted into the time passed to the host. "
            "Source Frames is the OpenFX convention: one time unit per media frame. "
            "Timeline Frames is for a host that resamples the input onto the output "
            "cadence before the effect sees it.");
  setString(paramProps, kOfxParamPropChoiceOption, 0, "Source Frames (OFX standard)");
  setString(paramProps, kOfxParamPropChoiceOption, 1, "Timeline Frames (host conformed)");
  setInt(paramProps, kOfxParamPropDefault, 0, 0);
  setInt(paramProps, kOfxParamPropAnimates, 0, 0);

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
            "Write the full temporal trace to the debugger and to the temporal log file");
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

OfxStatus fetchOptionalParam(OfxParamSetHandle paramSet, const char* name,
                             OfxParamHandle* handle) {
  return gParameterSuite->paramGetHandle(paramSet, name, handle, nullptr);
}

OfxStatus createInstance(OfxImageEffectHandle effect) {
  auto* data = new InstanceData();
  data->effect = effect;

  // kOfxActionCreateInstance carries no inArgs. kOfxImageEffectPropContext is a
  // read-only property of the effect instance itself.
  OfxPropertySetHandle instanceProps = nullptr;
  char* context = nullptr;
  if (gImageEffectSuite->getPropertySet(effect, &instanceProps) == kOfxStatOK &&
      instanceProps &&
      gPropertySuite->propGetString(instanceProps, kOfxImageEffectPropContext, 0,
                                    &context) == kOfxStatOK &&
      context) {
    data->retimerContext =
        std::strcmp(context, kOfxImageEffectContextRetimer) == 0;
    data->contextName = context;
    retimerProbeLog(std::string("createInstance context=") + context);
  } else {
    data->contextName = "unavailable";
    retimerProbeLog("createInstance context unavailable; assuming filter");
  }

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
    status = fetchOptionalParam(paramSet, kEnabledParam, &data->enabledParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kDetectedFrameRateParam,
                                &data->detectedFrameRateParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kSourceFrameRateParam,
                                &data->sourceFrameRateParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kUseTimelineFrameRateParam,
                                &data->useTimelineFrameRateParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kTargetFrameRateParam,
                                &data->targetFrameRateParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kSourceTimeBaseParam,
                                &data->sourceTimeBaseParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kModeParam, &data->modeParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kQualityParam, &data->qualityParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kDebugParam, &data->debugParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kModelParam, &data->modelParam);
  }
  if (status == kOfxStatOK) {
    status = fetchOptionalParam(paramSet, kGpuDeviceParam, &data->gpuDeviceParam);
  }

  if (status == kOfxStatOK) {
    // The mandated SourceTime pseudo parameter only exists in the retimer
    // context, and only that context defines it, so it is never looked up on a
    // plain filter instance.
    // Normally only the retimer context defines it. While probing, look it up
    // in every context: a host could in principle expose it elsewhere, and a
    // definitive "absent" is as much of an answer as a definitive "present".
    OfxStatus retimerParamStatus = kOfxStatErrUnknown;
    if (data->retimerContext || probeRetimerContext()) {
      retimerParamStatus = fetchOptionalParam(
          paramSet, kOfxImageEffectRetimerParamName, &data->sourceTimeParam);
      if (retimerParamStatus != kOfxStatOK) {
        data->sourceTimeParam = nullptr;
      }
    }

    std::ostringstream stream;
    stream << "SourceTime paramGetHandle status=" << retimerParamStatus
           << " present=" << (data->sourceTimeParam ? 1 : 0)
           << " retimerContext=" << (data->retimerContext ? 1 : 0)
           << " context=" << data->contextName;
    if (data->sourceTimeParam) {
      double initialSourceTime = 0.0;
      const OfxStatus valueStatus = gParameterSuite->paramGetValue(
          data->sourceTimeParam, &initialSourceTime);
      stream << " initialValueStatus=" << valueStatus
             << " initialValue=" << initialSourceTime;
    }
    retimerProbeLog(stream.str());
  }

  if (status == kOfxStatOK) {
    refreshClipTiming(data);
    logClipTiming(data);

    if (data->timing.sourceFrameRate > 0.0 && data->detectedFrameRateParam) {
      const OfxStatus setStatus = gParameterSuite->paramSetValue(
          data->detectedFrameRateParam, data->timing.sourceFrameRate);
      std::ostringstream stream;
      stream << "set detected frame rate=" << data->timing.sourceFrameRate
             << " status=" << setStatus;
      rifeofx::debugLog(stream.str());
    }
    if (data->timing.sourceFrameRate > 0.0 && data->sourceFrameRateParam) {
      const OfxStatus setStatus = gParameterSuite->paramSetValue(
          data->sourceFrameRateParam, data->timing.sourceFrameRate);
      std::ostringstream stream;
      stream << "set source frame rate default=" << data->timing.sourceFrameRate
             << " status=" << setStatus;
      rifeofx::debugLog(stream.str());
    }
    if (data->timing.outputFrameRate > 0.0 && data->targetFrameRateParam) {
      const OfxStatus setStatus = gParameterSuite->paramSetValue(
          data->targetFrameRateParam, data->timing.outputFrameRate);
      std::ostringstream stream;
      stream << "set target frame rate=" << data->timing.outputFrameRate
             << " status=" << setStatus;
      rifeofx::debugLog(stream.str());
    }

    // The anchor depends on the render time, so it cannot be resolved here.
    // The first render logs the one it actually used.

    data->temporalProvider = std::make_unique<rifeofx::TemporalFrameProvider>(
        gPropertySuite, gImageEffectSuite, data->sourceClip, effect,
        forceDebug());
    updateTargetFrameRateEnabled(data, 0.0);
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

OfxStatus instanceChanged(OfxImageEffectHandle effect,
                          OfxPropertySetHandle inArgs) {
  InstanceData* data = getInstanceData(effect);
  if (!data) {
    return kOfxStatErrBadHandle;
  }

  OfxTime time = 0.0;
  char* changedType = nullptr;
  char* changedName = nullptr;
  if (inArgs) {
    gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);
    gPropertySuite->propGetString(inArgs, kOfxPropType, 0, &changedType);
    gPropertySuite->propGetString(inArgs, kOfxPropName, 0, &changedName);
  }

  // A clip change can bring a different media cadence and a different frame
  // range, so the cached timing snapshot and the frame cache both have to go.
  if (changedType && std::strcmp(changedType, kOfxTypeClip) == 0) {
    refreshClipTiming(data);
    logClipTiming(data);
    if (data->temporalProvider) {
      data->temporalProvider->clear();
    }
    if (data->timing.sourceFrameRate > 0.0 && data->detectedFrameRateParam) {
      gParameterSuite->paramSetValue(data->detectedFrameRateParam,
                                     data->timing.sourceFrameRate);
    }
  } else if (changedName &&
             (std::strcmp(changedName, kSourceFrameRateParam) == 0 ||
              std::strcmp(changedName, kTargetFrameRateParam) == 0 ||
              std::strcmp(changedName, kUseTimelineFrameRateParam) == 0 ||
              std::strcmp(changedName, kSourceTimeBaseParam) == 0)) {
    // The cache is keyed by the requested time; a changed mapping asks for a
    // different set of times, so stale entries would never be hit anyway. It is
    // cleared to keep memory bounded during interactive tuning.
    if (data->temporalProvider) {
      data->temporalProvider->clear();
    }
  }

  updateTargetFrameRateEnabled(data, time);
  return kOfxStatOK;
}

OfxStatus getClipPreferences(OfxImageEffectHandle effect,
                             OfxPropertySetHandle outArgs) {
  InstanceData* data = getInstanceData(effect);
  if (!data || !outArgs) {
    return kOfxStatErrBadHandle;
  }

  // In the retimer context the host owns the output cadence and duration, so
  // republishing a frame rate here would only confound the probe.
  if (data->retimerContext) {
    retimerProbeLog("getClipPreferences: retimer context, not overriding the frame rate");
    return kOfxStatReplyDefault;
  }

  const double targetFrameRate = getTargetFrameRate(data, 0.0);
  if (targetFrameRate <= 0.0) {
    return kOfxStatReplyDefault;
  }
  const OfxStatus status = gPropertySuite->propSetDouble(
      outArgs, kOfxImageEffectPropFrameRate, 0, targetFrameRate);
  if (status != kOfxStatOK) {
    return status;
  }

  // Consecutive output frames may share one input source frame while their RIFE
  // timestep differs. Tell the host not to reuse an image merely because the
  // immediate input image is unchanged.
  const OfxStatus frameVaryingStatus = gPropertySuite->propSetInt(
      outArgs, kOfxImageEffectFrameVarying, 0, 1);
  if (getDebugParam(data, 0.0)) {
    std::ostringstream stream;
    stream << "clip preferences outputFrameRate=" << targetFrameRate
           << " frameRateStatus=" << status
           << " frameVaryingStatus=" << frameVaryingStatus;
    rifeofx::debugLog(stream.str());
  }
  return frameVaryingStatus == kOfxStatOK ? kOfxStatOK : frameVaryingStatus;
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

  // Only the explicit off switch short-circuits the effect. A zero timestep is
  // deliberately still rendered through the temporal path so the trace shows
  // which source frame the host actually returned.
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
  const InstanceData* data = getInstanceData(effect);
  if (!data) {
    return kOfxStatErrBadHandle;
  }

  OfxTime time = 0.0;
  if (gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time) != kOfxStatOK) {
    return kOfxStatErrBadHandle;
  }

  // A thumbnail render is not worth two source reads plus an inference.
  char* thumbnail = nullptr;
  const bool thumbnailRender =
      gPropertySuite->propGetString(inArgs, kOfxImageEffectPropThumbnailRender, 0,
                                    &thumbnail) == kOfxStatOK &&
      thumbnail && std::strcmp(thumbnail, "true") == 0;

  const rifeofx::TemporalMapping mapping = buildTemporalMapping(data, time);

  // The two source samples this output frame is built from, announced as one
  // continuous range. Under the default Source Frames time base these are the
  // integral media frames that bracket the source position.
  double sourceRange[2] = {mapping.sourceTimeA, mapping.sourceTimeB};
  if (thumbnailRender || mapping.policy == rifeofx::BlendPolicy::kHoldA) {
    sourceRange[1] = sourceRange[0];
  } else if (mapping.policy == rifeofx::BlendPolicy::kHoldB) {
    sourceRange[0] = sourceRange[1];
  }

  const OfxStatus status = gPropertySuite->propSetDoubleN(
      outArgs, "OfxImageClipPropFrameRange_Source", 2, sourceRange);
  if (getDebugParam(data, time)) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6)
           << "getFramesNeeded outputTime=" << time
           << " sourceRange=[" << sourceRange[0] << "," << sourceRange[1] << "]"
           << " policy=" << rifeofx::describeBlendPolicy(mapping.policy)
           << " thumbnail=" << (thumbnailRender ? 1 : 0)
           << " status=" << status;
    rifeofx::debugLog(stream.str());
    rifeofx::appendTemporalLog(stream.str());
  }
  return status;
}

OfxStatus render(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
  InstanceData* data = getInstanceData(effect);
  if (!data || !data->temporalProvider) {
    return kOfxStatErrBadHandle;
  }
  std::lock_guard<std::mutex> renderLock(data->renderMutex);

  OfxTime time = 0.0;
  OfxRectI renderWindow{};
  if (gPropertySuite->propGetDouble(inArgs, kOfxPropTime, 0, &time) != kOfxStatOK ||
      gPropertySuite->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4,
                                  &renderWindow.x1) != kOfxStatOK) {
    return kOfxStatErrBadHandle;
  }

  const bool debug = getDebugParam(data, time);
  if (probeRetimerContext()) {
    logRetimerProbe(data, time);
  }
  if (debug && !data->firstRenderLogged) {
    // Debug is usually switched on after the instance exists, so the timing
    // snapshot taken in createInstance never reaches the capture. Repeat it.
    data->firstRenderLogged = true;
    logClipTiming(data);
  }

  // 1. output time -> source position -> the two bracketing source frames.
  const rifeofx::TemporalMapping mapping = buildTemporalMapping(data, time);
  if (debug) {
    const std::string record = rifeofx::formatTemporalMapping(mapping);
    rifeofx::debugLogBlock(record);
    rifeofx::appendTemporalLog(record);
  }

  // 2. explicit temporal reads at those two times.
  TemporalInputs inputs;
  OfxStatus status = fetchTemporalInputs(data, mapping, debug, &inputs);
  if (status != kOfxStatOK || !inputs.frameA || !inputs.frameB) {
    return gImageEffectSuite->abort(effect)
               ? kOfxStatOK
               : (status == kOfxStatOK ? kOfxStatFailed : status);
  }
  if (debug && inputs.signaturesComputed) {
    std::ostringstream stream;
    stream << "frameA_debug_signature=" << rifeofx::toHex(inputs.signatureA)
           << " frameB_debug_signature=" << rifeofx::toHex(inputs.signatureB)
           << " identicalImages=" << (inputs.identicalImages ? 1 : 0);
    rifeofx::debugLog(stream.str());
    rifeofx::appendTemporalLog(stream.str());
  }
  if (debug && !data->renderTimeProbeDone) {
    data->renderTimeProbeDone = true;
    probeRenderTimeAddressing(data, time, mapping, inputs);
    probeInputCadence(data, time);
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

  const rifeofx::CachedFrame& frameA = *inputs.frameA;
  const rifeofx::CachedFrame& frameB = *inputs.frameB;

  // 3. the interpolation itself. A timestep pinned to 0 or 1, a clip edge, or a
  // missing neighbour all resolve to a direct copy instead of an inference.
  rifeofx::BlendPolicy policy = mapping.policy;
  if (inputs.neighbourUnavailable && policy != rifeofx::BlendPolicy::kHoldA) {
    policy = rifeofx::BlendPolicy::kHoldA;
  }

  std::vector<float> interpolated;
  const std::vector<float>* outputSource = nullptr;
  switch (policy) {
    case rifeofx::BlendPolicy::kHoldA:
      outputSource = &frameA.rgba;
      break;
    case rifeofx::BlendPolicy::kHoldB:
      outputSource = &frameB.rgba;
      break;
    case rifeofx::BlendPolicy::kInterpolate:
      status = runRifeInterpolation(data, time, frameA, frameB, mapping.timestep,
                                    debug, interpolated);
      if (status != kOfxStatOK) {
        gImageEffectSuite->clipReleaseImage(outputImage);
        return status;
      }
      outputSource = &interpolated;
      break;
  }

  // The interpolated buffer and both source frames share frame A's geometry.
  const OfxRectI& sourceBounds = policy == rifeofx::BlendPolicy::kHoldB
                                     ? frameB.bounds
                                     : frameA.bounds;
  const int sourceWidth = sourceBounds.x2 - sourceBounds.x1;
  const int x1 = std::max({renderWindow.x1, outputBounds.x1, sourceBounds.x1});
  const int y1 = std::max({renderWindow.y1, outputBounds.y1, sourceBounds.y1});
  const int x2 = std::min({renderWindow.x2, outputBounds.x2, sourceBounds.x2});
  const int y2 = std::min({renderWindow.y2, outputBounds.y2, sourceBounds.y2});

  if (x1 < x2 && y1 < y2 && outputSource) {
    constexpr int channels = 4;
    for (int y = y1; y < y2; ++y) {
      if (gImageEffectSuite->abort(effect)) {
        break;
      }
      auto* outputRow = static_cast<float*>(outputData) +
                        ((y - outputBounds.y1) * outputRowBytes / sizeof(float)) +
                        (x1 - outputBounds.x1) * channels;
      const float* sourceRow = outputSource->data() +
                               static_cast<size_t>(y - sourceBounds.y1) * sourceWidth * channels +
                               static_cast<size_t>(x1 - sourceBounds.x1) * channels;
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

// Per-frame actions are excluded: the probe needs the lifecycle, not a flood.
bool isPerFrameAction(const char* action) {
  return std::strcmp(action, kOfxImageEffectActionRender) == 0 ||
         std::strcmp(action, kOfxImageEffectActionIsIdentity) == 0 ||
         std::strcmp(action, kOfxImageEffectActionGetFramesNeeded) == 0 ||
         std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0 ||
         std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0;
}

OfxStatus pluginMain(const char* action, const void* handle,
                     OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs) {
  // Traces how far the host got before it stopped talking to us, which is what
  // tells a rejected context apart from a context that was never offered.
  if (probeRetimerContext() && !isPerFrameAction(action)) {
    retimerProbeLog(std::string("action=") + action);
  }

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
  if (std::strcmp(action, kOfxActionInstanceChanged) == 0) {
    return instanceChanged(static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)),
                           inArgs);
  }
  if (std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
    return getClipPreferences(
        static_cast<OfxImageEffectHandle>(const_cast<void*>(handle)), outArgs);
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
  if (probeRetimerContext()) {
    retimerProbeLog("OfxGetNumberOfPlugins -> 1");
  }
  return 1;
}

extern "C" __declspec(dllexport) OfxPlugin* OfxGetPlugin(int nth) {
  if (probeRetimerContext()) {
    retimerProbeLog("OfxGetPlugin nth=" + std::to_string(nth));
  }
  return nth == 0 ? &gPlugin : nullptr;
}
