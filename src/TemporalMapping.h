#pragma once

#include <cstdint>
#include <string>

#include <ofxCore.h>

namespace rifeofx {

// What render should do with the pair of source images.
enum class BlendPolicy {
  kHoldA = 0,       // output == source frame A, no inference
  kHoldB = 1,       // output == source frame B, no inference
  kInterpolate = 2, // RIFE(A, B, timestep)
};

// One output frame resolved to the source images it is built from. Produced by
// the cadence layer (see CadenceMapping.h) and consumed by render and
// getFramesNeeded, which must agree about which images are needed.
struct TemporalMapping {
  OfxTime outputTime = 0.0;
  std::int64_t outputFrame = 0;
  double sourceFrameRate = 0.0;
  double outputFrameRate = 0.0;
  double sourceSeconds = 0.0;
  double sourcePosition = 0.0;
  // Timeline time the source position is measured from: the calibrated phase of
  // the host conform.
  OfxTime timelineOrigin = 0.0;
  std::int64_t sourceFrameA = 0;
  std::int64_t sourceFrameB = 0;
  // Timeline times carrying those two original images.
  OfxTime sourceTimeA = 0.0;
  OfxTime sourceTimeB = 0.0;
  float timestep = 0.0f;
  BlendPolicy policy = BlendPolicy::kHoldA;
  bool ratesValid = false;
  // False when the mapping could not be established and the render is a safe
  // hold instead. reason says why.
  bool resolved = true;
  const char* reason = "default";

  bool needsInference() const { return policy == BlendPolicy::kInterpolate; }
  // True when the render only consumes a single source image.
  bool singleImage() const { return sourceTimeA == sourceTimeB; }
};

const char* describeBlendPolicy(BlendPolicy policy);

}  // namespace rifeofx
