#include "TemporalMapping.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

int gFailures = 0;

bool approximatelyEqual(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) < tolerance;
}

void expect(bool condition, const char* message) {
  if (!condition) {
    ++gFailures;
    std::cerr << "TemporalMappingTest failure: " << message << "\n";
  }
}

rifeofx::TemporalMappingInputs baseInputs(double outputTime, double sourceFps,
                                          double outputFps) {
  rifeofx::TemporalMappingInputs inputs;
  inputs.outputTime = outputTime;
  inputs.sourceFrameRate = sourceFps;
  inputs.outputFrameRate = outputFps;
  return inputs;
}

// 50 fps media on a 60 fps timeline: the reference case from the test plan.
void test50to60() {
  const std::array<double, 7> kExpectedPositions =
      {0.0, 5.0 / 6.0, 5.0 / 3.0, 2.5, 10.0 / 3.0, 25.0 / 6.0, 5.0};
  const std::array<std::int64_t, 7> kExpectedFrameA = {0, 0, 1, 2, 3, 4, 5};
  const std::array<double, 7> kExpectedTimesteps =
      {0.0, 5.0 / 6.0, 2.0 / 3.0, 0.5, 1.0 / 3.0, 1.0 / 6.0, 0.0};

  for (std::size_t frame = 0; frame < kExpectedPositions.size(); ++frame) {
    const rifeofx::TemporalMapping mapping = rifeofx::computeTemporalMapping(
        baseInputs(static_cast<double>(frame), 50.0, 60.0));
    expect(approximatelyEqual(mapping.sourcePosition, kExpectedPositions[frame]),
           "50->60 sourcePosition");
    expect(mapping.sourceFrameA == kExpectedFrameA[frame], "50->60 sourceFrameA");
    expect(mapping.sourceFrameB == kExpectedFrameA[frame] + 1, "50->60 sourceFrameB");
    expect(approximatelyEqual(mapping.timestep, kExpectedTimesteps[frame], 1e-5),
           "50->60 timestep");
    // The default policy addresses the input clip in its own frame numbering,
    // so every request must be an integral media frame.
    expect(approximatelyEqual(mapping.sourceTimeA,
                              static_cast<double>(kExpectedFrameA[frame])),
           "50->60 sourceTimeA is the integral source frame");
    const bool onSourceFrame = frame == 0 || frame == 6;
    expect(mapping.needsInference() == !onSourceFrame,
           "50->60 inference only between source frames");
  }
}

// The same cadence expressed on the timeline axis: source frames are 1.2 output
// time units apart and requests become fractional.
void testTimelineTimeBase() {
  rifeofx::TemporalMappingInputs inputs = baseInputs(3.0, 50.0, 60.0);
  inputs.timeBase = rifeofx::SourceTimeBase::kTimelineFrames;
  const rifeofx::TemporalMapping mapping = rifeofx::computeTemporalMapping(inputs);
  expect(approximatelyEqual(mapping.sourceTimeA, 2.4), "timeline base sourceTimeA");
  expect(approximatelyEqual(mapping.sourceTimeB, 3.6), "timeline base sourceTimeB");
  expect(approximatelyEqual(mapping.timestep, 0.5, 1e-5), "timeline base timestep");
}

// Resolve reports render times on a composition-global axis.
void testNonZeroOrigin() {
  rifeofx::TemporalMappingInputs inputs = baseInputs(6978.0, 50.0, 60.0);
  inputs.timelineOrigin = 6975.0;
  const rifeofx::TemporalMapping mapping = rifeofx::computeTemporalMapping(inputs);
  expect(approximatelyEqual(mapping.sourcePosition, 2.5), "origin sourcePosition");
  expect(approximatelyEqual(mapping.sourceTimeA, 6977.0), "origin sourceTimeA");
  expect(approximatelyEqual(mapping.sourceTimeB, 6978.0), "origin sourceTimeB");
  expect(approximatelyEqual(mapping.timestep, 0.5, 1e-5), "origin timestep");
}

// Identical rates must never trigger inference, including the NTSC rates where
// the ratio is only integral after the floating point residue is snapped away.
void testMatchedRates() {
  const double rates[] = {23.976023976023978, 29.97002997002997,
                          59.94005994005994, 25.0, 60.0};
  for (const double rate : rates) {
    for (int frame = 0; frame < 200; frame += 37) {
      const rifeofx::TemporalMapping mapping = rifeofx::computeTemporalMapping(
          baseInputs(static_cast<double>(frame), rate, rate));
      expect(mapping.policy == rifeofx::BlendPolicy::kHoldA,
             "matched rates must pass through");
      expect(mapping.sourceFrameA == frame, "matched rates frame index");
    }
  }
}

// 23.976 -> 59.94 is an exact 1:2.5 ratio, so every second output frame lands on
// a real source frame while the others sit at a fixed fraction.
void testNtscUpconversion() {
  const double sourceFps = 24000.0 / 1001.0;
  const double outputFps = 60000.0 / 1001.0;
  const rifeofx::TemporalMapping onFrame =
      rifeofx::computeTemporalMapping(baseInputs(5.0, sourceFps, outputFps));
  expect(approximatelyEqual(onFrame.sourcePosition, 2.0), "ntsc integral position");
  expect(onFrame.policy == rifeofx::BlendPolicy::kHoldA, "ntsc integral hold");

  const rifeofx::TemporalMapping fractional =
      rifeofx::computeTemporalMapping(baseInputs(4.0, sourceFps, outputFps));
  expect(approximatelyEqual(fractional.sourcePosition, 1.6), "ntsc fractional position");
  expect(approximatelyEqual(fractional.timestep, 0.6, 1e-5), "ntsc fractional timestep");
}

void testEdges() {
  rifeofx::TemporalMappingInputs inputs = baseInputs(1.0, 50.0, 60.0);
  inputs.clampToSourceRange = true;
  inputs.sourceFirstFrame = 0.0;
  inputs.sourceLastFrame = 100.0;

  // Before the first media frame the first frame is held, never extrapolated.
  inputs.outputTime = -3.0;
  const rifeofx::TemporalMapping start = rifeofx::computeTemporalMapping(inputs);
  expect(start.clampedAtStart, "clamp before first frame");
  expect(approximatelyEqual(start.sourceTimeA, 0.0), "clamped start time");
  expect(start.singleImage(), "clamped start uses one image");
  expect(!start.needsInference(), "clamped start skips inference");

  // The trailing neighbour of the last media frame does not exist.
  inputs.outputTime = 121.0;  // sourcePosition ~100.83
  const rifeofx::TemporalMapping end = rifeofx::computeTemporalMapping(inputs);
  expect(end.clampedAtEnd, "clamp past last frame");
  expect(approximatelyEqual(end.sourceTimeA, 100.0), "clamped end time");
  expect(end.singleImage(), "clamped end uses one image");
  expect(!end.needsInference(), "clamped end skips inference");

  // Without a usable range the request is left untouched for the host to reject.
  inputs.clampToSourceRange = false;
  const rifeofx::TemporalMapping unclamped = rifeofx::computeTemporalMapping(inputs);
  expect(!unclamped.clampedAtEnd, "no clamping without a usable range");
  expect(unclamped.sourceFrameB == 101, "unclamped neighbour index");
}

void testHoldEpsilon() {
  rifeofx::TemporalMappingInputs inputs = baseInputs(0.0, 50.0, 60.0);
  inputs.holdEpsilon = 1e-3;

  // A position a hair above an integer must render the source frame itself.
  inputs.outputTime = 1.2 * (1.0 + 1e-7);
  const rifeofx::TemporalMapping nearZero = rifeofx::computeTemporalMapping(inputs);
  expect(nearZero.policy == rifeofx::BlendPolicy::kHoldA, "near-zero timestep holds A");

  // A position a hair below the next integer must render that next frame.
  inputs.outputTime = 2.4 * (1.0 - 1e-7);
  const rifeofx::TemporalMapping nearOne = rifeofx::computeTemporalMapping(inputs);
  expect(nearOne.policy == rifeofx::BlendPolicy::kHoldB, "near-one timestep holds B");
  expect(nearOne.sourceFrameB == 2, "near-one neighbour index");
}

// Regression: Resolve reported a media-local Source range of 0..1999 while
// rendering at timeline time 6816. Clamping against that range pinned every
// output frame to source time 1999, which froze the picture. The caller decides
// whether the range is on the render axis; when it says no, nothing may be
// clamped.
void testForeignAxisRangeIsNotClamped() {
  rifeofx::TemporalMappingInputs inputs = baseInputs(6816.0, 50.0, 60.0);
  inputs.timelineOrigin = 6816.0;
  inputs.clampToSourceRange = false;
  inputs.sourceFirstFrame = 0.0;
  inputs.sourceLastFrame = 1999.0;

  const rifeofx::TemporalMapping mapping = rifeofx::computeTemporalMapping(inputs);
  expect(!mapping.clampedAtEnd, "a foreign-axis range must not clamp");
  expect(approximatelyEqual(mapping.sourceTimeA, 6816.0),
         "unclamped request stays on the render axis");
  expect(mapping.timelineOrigin == 6816.0, "origin is reported back");
  expect(!mapping.clampEnabled, "clampEnabled mirrors the caller's decision");

  // Six output frames later the mapping must have advanced five source frames.
  inputs.outputTime = 6822.0;
  const rifeofx::TemporalMapping advanced = rifeofx::computeTemporalMapping(inputs);
  expect(approximatelyEqual(advanced.sourcePosition, 5.0), "advanced position");
  expect(approximatelyEqual(advanced.sourceTimeA, 6821.0), "advanced source time");
  expect(!advanced.clampedAtEnd, "advanced request is still unclamped");
}

// When no reported range locates the clip, the caller anchors at the render
// time. That must degrade to a passthrough, not to a request far from the
// playhead.
void testUnresolvedAnchorIsPassthrough() {
  rifeofx::TemporalMappingInputs inputs = baseInputs(6816.0, 50.0, 60.0);
  inputs.timelineOrigin = 6816.0;
  inputs.anchorResolved = false;
  inputs.anchorReason = "unresolved";

  const rifeofx::TemporalMapping mapping = rifeofx::computeTemporalMapping(inputs);
  expect(approximatelyEqual(mapping.sourcePosition, 0.0), "unresolved position");
  expect(approximatelyEqual(mapping.sourceTimeA, 6816.0), "unresolved passthrough time");
  expect(mapping.policy == rifeofx::BlendPolicy::kHoldA, "unresolved holds one image");
  expect(!mapping.anchorResolved, "unresolved flag survives into the mapping");
}

void testDegenerateRates() {
  const rifeofx::TemporalMapping mapping =
      rifeofx::computeTemporalMapping(baseInputs(42.0, 0.0, 60.0));
  expect(!mapping.ratesValid, "missing source rate is reported");
  expect(approximatelyEqual(mapping.sourceTimeA, 42.0),
         "degenerate configuration falls back to the render time");
  expect(mapping.singleImage(), "degenerate configuration uses one image");
}

}  // namespace

int main() {
  test50to60();
  testTimelineTimeBase();
  testNonZeroOrigin();
  testMatchedRates();
  testNtscUpconversion();
  testEdges();
  testHoldEpsilon();
  testForeignAxisRangeIsNotClamped();
  testUnresolvedAnchorIsPassthrough();
  testDegenerateRates();

  if (gFailures == 0) {
    std::cout << "TemporalMappingTest: all checks passed\n";
  }
  return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
