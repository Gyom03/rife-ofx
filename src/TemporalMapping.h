#pragma once

#include <cstdint>
#include <string>

#include <ofxCore.h>

namespace rifeofx {

// How a source frame index is turned back into the OfxTime handed to
// OfxImageEffectSuiteV1::clipGetImage.
//
// kSourceFrames is the OpenFX convention used by the SDK retimer example
// (Support/Plugins/Retimer/retimer.cpp): the input clip is addressed in its own
// frame numbering, one OfxTime unit per source frame, so a request is always an
// integer and always names a real media frame.
//
// kTimelineFrames is the alternative to test when a host resamples the input
// clip onto the output cadence before the effect sees it. Source frame N then
// sits at outputFrameRate / sourceFrameRate time units apart on the timeline
// axis, and requests become fractional.
enum class SourceTimeBase {
  kSourceFrames = 0,
  kTimelineFrames = 1,
};

// What render should do with the pair of source images.
enum class BlendPolicy {
  kHoldA = 0,       // output == source frame A, no inference
  kHoldB = 1,       // output == source frame B, no inference
  kInterpolate = 2, // RIFE(A, B, timestep)
};

struct TemporalMappingInputs {
  OfxTime outputTime = 0.0;
  // OfxTime that corresponds to source position 0. Resolve reports render
  // times on a composition-global axis, so this is not assumed to be zero.
  OfxTime timelineOrigin = 0.0;
  double sourceFrameRate = 0.0;
  double outputFrameRate = 0.0;
  SourceTimeBase timeBase = SourceTimeBase::kSourceFrames;

  // Clamping is only applied when the host reported a frame range that is
  // expressed on the same axis as the render time. A media-local range must not
  // be used to clamp a composition-global request.
  bool clampToSourceRange = false;
  OfxTime sourceFirstFrame = 0.0;
  OfxTime sourceLastFrame = 0.0;

  // Whether timelineOrigin actually located the clip on the render axis, and
  // why. When it did not, the mapping degrades to a passthrough instead of
  // asking for a source frame far away from the playhead.
  bool anchorResolved = true;
  const char* anchorReason = "default";

  // Below this distance from 0 or 1 the fractional position is treated as an
  // exact source frame and RIFE is skipped.
  double holdEpsilon = 1e-4;

  // In the OpenFX retimer context the host owns the mapping and publishes the
  // position, in source frames, through kOfxImageEffectRetimerParamName. When
  // that value is available it replaces the rate-derived position entirely.
  bool hostSourcePositionValid = false;
  double hostSourcePosition = 0.0;
};

struct TemporalMapping {
  OfxTime outputTime = 0.0;
  std::int64_t outputFrame = 0;
  double sourceFrameRate = 0.0;
  double outputFrameRate = 0.0;
  double sourceSeconds = 0.0;
  double sourcePosition = 0.0;
  OfxTime timelineOrigin = 0.0;
  std::int64_t sourceFrameA = 0;
  std::int64_t sourceFrameB = 0;
  OfxTime sourceTimeA = 0.0;
  OfxTime sourceTimeB = 0.0;
  float timestep = 0.0f;
  BlendPolicy policy = BlendPolicy::kHoldA;
  SourceTimeBase timeBase = SourceTimeBase::kSourceFrames;
  bool ratesValid = false;
  bool hostProvidedPosition = false;
  bool clampEnabled = false;
  bool anchorResolved = true;
  const char* anchorReason = "default";
  bool clampedAtStart = false;
  bool clampedAtEnd = false;

  bool needsInference() const { return policy == BlendPolicy::kInterpolate; }
  // True when the render only consumes a single source image.
  bool singleImage() const { return sourceTimeA == sourceTimeB; }
};

TemporalMapping computeTemporalMapping(const TemporalMappingInputs& inputs);

const char* describeSourceTimeBase(SourceTimeBase timeBase);
const char* describeBlendPolicy(BlendPolicy policy);

// The canonical multi-line DebugView record for one output frame.
std::string formatTemporalMapping(const TemporalMapping& mapping);

}  // namespace rifeofx
