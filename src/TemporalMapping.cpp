#include "TemporalMapping.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace rifeofx {

namespace {

// Snap a position that is integral up to floating point residue. Without this a
// 60 fps output frame that lands exactly on a 50 fps source frame can compute
// 4.999999999999 and be rendered as an almost-identity RIFE frame instead of
// being passed through. The tolerance is relative so it stays meaningful on a
// composition-global timeline (Resolve render times in the tens of thousands).
double snapToInteger(double value) {
  const double nearest = std::round(value);
  const double tolerance = std::max(1e-9, std::abs(value) * 1e-12);
  return std::abs(value - nearest) < tolerance ? nearest : value;
}

std::int64_t toFrameIndex(double value) {
  return static_cast<std::int64_t>(std::llround(std::floor(value)));
}

}  // namespace

TemporalMapping computeTemporalMapping(const TemporalMappingInputs& inputs) {
  TemporalMapping mapping;
  mapping.outputTime = inputs.outputTime;
  mapping.timeBase = inputs.timeBase;
  mapping.timelineOrigin = inputs.timelineOrigin;
  mapping.clampEnabled = inputs.clampToSourceRange;
  mapping.anchorResolved = inputs.anchorResolved;
  mapping.anchorReason = inputs.anchorReason ? inputs.anchorReason : "default";
  mapping.sourceFrameRate = inputs.sourceFrameRate;
  mapping.outputFrameRate = inputs.outputFrameRate;
  mapping.outputFrame = std::isfinite(inputs.outputTime)
                            ? static_cast<std::int64_t>(std::llround(inputs.outputTime))
                            : 0;

  const bool usableRates = std::isfinite(inputs.outputTime) &&
                           inputs.sourceFrameRate > 0.0 &&
                           inputs.outputFrameRate > 0.0;
  const bool hostPosition = inputs.hostSourcePositionValid &&
                            std::isfinite(inputs.hostSourcePosition);
  if (!usableRates && !hostPosition) {
    // Degenerate configuration: behave as a plain filter on the requested time
    // rather than inventing a cadence the host never reported.
    mapping.sourceTimeA = inputs.outputTime;
    mapping.sourceTimeB = inputs.outputTime;
    mapping.sourceFrameA = mapping.outputFrame;
    mapping.sourceFrameB = mapping.outputFrame;
    mapping.policy = BlendPolicy::kHoldA;
    return mapping;
  }
  mapping.ratesValid = usableRates;
  mapping.hostProvidedPosition = hostPosition;

  if (hostPosition) {
    // The host already resolved output time to a source position.
    mapping.sourcePosition = snapToInteger(inputs.hostSourcePosition);
    mapping.sourceSeconds = usableRates
                                ? mapping.sourcePosition / inputs.sourceFrameRate
                                : 0.0;
  } else {
    // Output time -> seconds inside the effect -> position in source frames.
    const double outputOffset = inputs.outputTime - inputs.timelineOrigin;
    mapping.sourceSeconds = outputOffset / inputs.outputFrameRate;
    mapping.sourcePosition =
        snapToInteger(mapping.sourceSeconds * inputs.sourceFrameRate);
  }

  const double lowerPosition = std::floor(mapping.sourcePosition);
  double fraction = mapping.sourcePosition - lowerPosition;

  mapping.sourceFrameA = toFrameIndex(mapping.sourcePosition);
  mapping.sourceFrameB = mapping.sourceFrameA + 1;

  // Source frame index -> OfxTime. This is the step the OpenFX spec leaves to
  // the host/plugin agreement, so it is an explicit, switchable policy.
  const double sourceFrameStep =
      inputs.timeBase == SourceTimeBase::kTimelineFrames && usableRates
          ? inputs.outputFrameRate / inputs.sourceFrameRate
          : 1.0;
  mapping.sourceTimeA = inputs.timelineOrigin + lowerPosition * sourceFrameStep;
  mapping.sourceTimeB = mapping.sourceTimeA + sourceFrameStep;

  if (inputs.clampToSourceRange &&
      inputs.sourceFirstFrame <= inputs.sourceLastFrame) {
    if (mapping.sourceTimeA < inputs.sourceFirstFrame) {
      // Before the first media frame: hold it instead of asking the host for an
      // image that does not exist.
      mapping.sourceTimeA = inputs.sourceFirstFrame;
      mapping.sourceTimeB = inputs.sourceFirstFrame;
      fraction = 0.0;
      mapping.clampedAtStart = true;
    } else if (mapping.sourceTimeB > inputs.sourceLastFrame) {
      // The trailing neighbour is past the end of the media. Hold the last
      // frame that does exist rather than repeating an unrelated image.
      mapping.sourceTimeA = std::min(mapping.sourceTimeA, inputs.sourceLastFrame);
      mapping.sourceTimeB = mapping.sourceTimeA;
      fraction = 0.0;
      mapping.clampedAtEnd = true;
    }
  }

  const double holdEpsilon = inputs.holdEpsilon > 0.0 ? inputs.holdEpsilon : 0.0;
  if (mapping.sourceTimeA == mapping.sourceTimeB || fraction <= holdEpsilon) {
    mapping.timestep = 0.0f;
    mapping.policy = BlendPolicy::kHoldA;
  } else if (fraction >= 1.0 - holdEpsilon) {
    mapping.timestep = 1.0f;
    mapping.policy = BlendPolicy::kHoldB;
  } else {
    mapping.timestep = static_cast<float>(fraction);
    mapping.policy = BlendPolicy::kInterpolate;
  }
  return mapping;
}

const char* describeSourceTimeBase(SourceTimeBase timeBase) {
  switch (timeBase) {
    case SourceTimeBase::kSourceFrames:
      return "sourceFrames";
    case SourceTimeBase::kTimelineFrames:
      return "timelineFrames";
  }
  return "unknown";
}

const char* describeBlendPolicy(BlendPolicy policy) {
  switch (policy) {
    case BlendPolicy::kHoldA:
      return "holdA";
    case BlendPolicy::kHoldB:
      return "holdB";
    case BlendPolicy::kInterpolate:
      return "interpolate";
  }
  return "unknown";
}

std::string formatTemporalMapping(const TemporalMapping& mapping) {
  std::ostringstream stream;
  stream << std::fixed;
  stream << "[RifeOFX]\n";
  stream << "outputTime=" << std::setprecision(3) << mapping.outputTime << "\n";
  stream << "outputFrame=" << mapping.outputFrame << "\n";
  stream << "sourceFPS=" << std::setprecision(3) << mapping.sourceFrameRate << "\n";
  stream << "outputFPS=" << std::setprecision(3) << mapping.outputFrameRate << "\n";
  stream << "sourcePosition=" << std::setprecision(6) << mapping.sourcePosition << "\n";
  stream << "timelineOrigin=" << std::setprecision(3) << mapping.timelineOrigin << "\n";
  stream << "sourceFrameA=" << mapping.sourceFrameA << "\n";
  stream << "sourceFrameB=" << mapping.sourceFrameB << "\n";
  stream << "sourceTimeA=" << std::setprecision(6) << mapping.sourceTimeA << "\n";
  stream << "sourceTimeB=" << std::setprecision(6) << mapping.sourceTimeB << "\n";
  stream << "timestep=" << std::setprecision(6) << mapping.timestep << "\n";
  stream << "timeBase=" << describeSourceTimeBase(mapping.timeBase) << "\n";
  stream << "policy=" << describeBlendPolicy(mapping.policy) << "\n";
  stream << "anchor=" << mapping.anchorReason
         << " anchorResolved=" << (mapping.anchorResolved ? 1 : 0)
         << " clampEnabled=" << (mapping.clampEnabled ? 1 : 0) << "\n";
  stream << "clampedAtStart=" << (mapping.clampedAtStart ? 1 : 0)
         << " clampedAtEnd=" << (mapping.clampedAtEnd ? 1 : 0)
         << " ratesValid=" << (mapping.ratesValid ? 1 : 0)
         << " hostProvidedPosition=" << (mapping.hostProvidedPosition ? 1 : 0);
  return stream.str();
}

}  // namespace rifeofx
