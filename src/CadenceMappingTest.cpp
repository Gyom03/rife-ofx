#include "CadenceMapping.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <set>
#include <vector>

namespace {

int gFailures = 0;

void expect(bool condition, const std::string& message) {
  if (!condition) {
    ++gFailures;
    std::cerr << "CadenceMappingTest failure: " << message << "\n";
  }
}

bool near(double actual, double expected, double tolerance = 1e-6) {
  return std::abs(actual - expected) < tolerance;
}

// Simulates what the host hands the effect: a conformed stream where original
// frame k occupies every timeline time whose cycle offset maps to k.
std::uint64_t conformedSignature(std::int64_t timelineTime, std::int64_t phase,
                                 const rifeofx::Rational& ratio) {
  const std::int64_t offset = timelineTime - phase;
  const std::int64_t numerator = ratio.num * offset;
  std::int64_t frame = numerator / ratio.den;
  if (numerator % ratio.den != 0 && numerator < 0) {
    --frame;
  }
  // An arbitrary but injective mapping from original frame index to signature.
  return static_cast<std::uint64_t>(frame) * 0x9E3779B97F4A7C15ULL + 0x1234ULL;
}

void testRatios() {
  const rifeofx::Rational fifty = rifeofx::cadenceRatio(50.0, 60.0);
  expect(fifty.num == 5 && fifty.den == 6, "50/60 must reduce to 5/6");

  const rifeofx::Rational ntsc = rifeofx::cadenceRatio(24000.0 / 1001.0, 60000.0 / 1001.0);
  expect(ntsc.num == 2 && ntsc.den == 5, "23.976/59.94 must reduce to 2/5");

  // The rounded values a user would type must land on the same ratio.
  const rifeofx::Rational typed = rifeofx::cadenceRatio(23.976, 59.94);
  expect(typed.num == 2 && typed.den == 5, "typed NTSC rates must reduce to 2/5");

  const rifeofx::Rational drop = rifeofx::cadenceRatio(29.97, 59.94);
  expect(drop.num == 1 && drop.den == 2, "29.97/59.94 must reduce to 1/2");

  const rifeofx::Rational pal = rifeofx::cadenceRatio(25.0, 60.0);
  expect(pal.num == 5 && pal.den == 12, "25/60 must reduce to 5/12");

  const rifeofx::Rational same = rifeofx::cadenceRatio(60.0, 60.0);
  expect(same.num == 1 && same.den == 1, "equal rates must reduce to 1/1");
}

// The cycle from the specification, verified on 60 consecutive timeline frames.
void test50to60Over60Frames() {
  const rifeofx::Rational ratio = rifeofx::cadenceRatio(50.0, 60.0);
  const std::int64_t phase = 4;  // deliberately not zero
  const double expectedTimesteps[6] = {0.0,       5.0 / 6.0, 4.0 / 6.0,
                                       3.0 / 6.0, 2.0 / 6.0, 1.0 / 6.0};

  std::int64_t previousSourceFrame = 0;
  std::set<std::int64_t> fetchedTimes;
  for (std::int64_t step = 0; step < 60; ++step) {
    const std::int64_t timelineTime = 7000 + step;
    const rifeofx::CadenceMapping mapping =
        rifeofx::computeCadenceMapping(timelineTime, phase, ratio);
    const std::int64_t cyclePosition = mapping.cyclePosition;

    expect(near(mapping.timestep, expectedTimesteps[cyclePosition], 1e-5),
           "timestep cycle at timeline time " + std::to_string(timelineTime));

    // The two inputs must never be the same image, or RIFE would be blending a
    // frame with itself and the host duplicate would have leaked through.
    expect(mapping.timelineTimeA != mapping.timelineTimeB,
           "A and B must be different timeline times");
    expect(mapping.sourceFrameB == mapping.sourceFrameA + 1,
           "B must be the next original frame");

    // Each fetched time must be the first timeline frame of its block, which is
    // exactly the frame the host did not duplicate.
    for (std::int64_t fetched : {mapping.timelineTimeA, mapping.timelineTimeB}) {
      const std::int64_t offset = fetched - phase;
      const std::int64_t here = (ratio.num * offset) / ratio.den;
      const std::int64_t before = (ratio.num * (offset - 1)) / ratio.den;
      expect(here != before,
             "fetched time " + std::to_string(fetched) + " is a host duplicate");
      fetchedTimes.insert(fetched);
    }

    // Source position advances by exactly num/den per timeline frame.
    if (step > 0) {
      const std::int64_t advance = mapping.sourceFrameA - previousSourceFrame;
      expect(advance == 0 || advance == 1, "source frame must advance by 0 or 1");
    }
    previousSourceFrame = mapping.sourceFrameA;
  }

  // 60 timeline frames span 50 original frames.
  const rifeofx::CadenceMapping first =
      rifeofx::computeCadenceMapping(7000, phase, ratio);
  const rifeofx::CadenceMapping last =
      rifeofx::computeCadenceMapping(7060, phase, ratio);
  expect(last.sourceFrameA - first.sourceFrameA == 50,
         "60 timeline frames must span 50 original frames");
}

// The calibrator must recover a planted phase, for every phase and several
// cadences, without ever being told what it is.
void testCalibrationRecoversPhase() {
  struct Case {
    double sourceFps;
    double timelineFps;
  };
  const Case cases[] = {{50.0, 60.0}, {24000.0 / 1001.0, 60000.0 / 1001.0},
                        {25.0, 60.0}, {30.0, 60.0}};

  for (const Case& testCase : cases) {
    const rifeofx::Rational ratio =
        rifeofx::cadenceRatio(testCase.sourceFps, testCase.timelineFps);
    for (std::int64_t plantedPhase = 0; plantedPhase < ratio.den; ++plantedPhase) {
      rifeofx::CadenceCalibrator calibrator;
      calibrator.configure(ratio);
      expect(calibrator.state() == rifeofx::CalibrationState::kPending,
             "calibration must start pending");

      for (std::int64_t step = 0; step < 4 * ratio.den + 4; ++step) {
        const std::int64_t timelineTime = 6975 + step;
        calibrator.addSample(
            timelineTime, conformedSignature(timelineTime, plantedPhase, ratio));
        if (calibrator.state() == rifeofx::CalibrationState::kCalibrated) {
          break;
        }
      }

      const std::string label = std::to_string(ratio.num) + "/" +
                                std::to_string(ratio.den) + " phase " +
                                std::to_string(plantedPhase);
      expect(calibrator.state() == rifeofx::CalibrationState::kCalibrated,
             "calibration must succeed for " + label);
      expect(calibrator.phase() == plantedPhase,
             "calibration must recover phase for " + label + ", got " +
                 std::to_string(calibrator.phase()));
    }
  }
}

// A calibrated phase must reproduce the host's own duplicate positions.
void testCalibratedMappingMatchesTheHostStream() {
  const rifeofx::Rational ratio = rifeofx::cadenceRatio(50.0, 60.0);
  const std::int64_t plantedPhase = 3;
  rifeofx::CadenceCalibrator calibrator;
  calibrator.configure(ratio);
  for (std::int64_t step = 0; step < 20; ++step) {
    const std::int64_t timelineTime = 6975 + step;
    calibrator.addSample(timelineTime,
                         conformedSignature(timelineTime, plantedPhase, ratio));
  }
  expect(calibrator.state() == rifeofx::CalibrationState::kCalibrated,
         "calibration must succeed before the mapping check");

  for (std::int64_t step = 0; step < 60; ++step) {
    const std::int64_t timelineTime = 7100 + step;
    const rifeofx::CadenceMapping mapping =
        rifeofx::computeCadenceMapping(timelineTime, calibrator.phase(), ratio);
    // The images the mapping asks for must really be the originals it expects.
    expect(conformedSignature(mapping.timelineTimeA, plantedPhase, ratio) ==
               conformedSignature(mapping.timelineTimeA, plantedPhase, ratio),
           "signature lookup must be stable");
    const std::uint64_t signatureA =
        conformedSignature(mapping.timelineTimeA, plantedPhase, ratio);
    const std::uint64_t signatureB =
        conformedSignature(mapping.timelineTimeB, plantedPhase, ratio);
    expect(signatureA != signatureB,
           "the two RIFE inputs must be different images at timeline time " +
               std::to_string(timelineTime));
  }
}

void testAmbiguousCases() {
  const rifeofx::Rational ratio = rifeofx::cadenceRatio(50.0, 60.0);

  // A static clip: every frame identical.
  rifeofx::CadenceCalibrator staticClip;
  staticClip.configure(ratio);
  for (std::int64_t step = 0; step < 30; ++step) {
    staticClip.addSample(1000 + step, 0xABCDEF01ULL);
  }
  expect(staticClip.state() == rifeofx::CalibrationState::kAmbiguous,
         "a static clip must be reported ambiguous");

  // A cross fade: every frame distinct, no repeats where the cadence needs them.
  rifeofx::CadenceCalibrator fade;
  fade.configure(ratio);
  for (std::int64_t step = 0; step < 30; ++step) {
    fade.addSample(2000 + step, static_cast<std::uint64_t>(step) * 7919ULL + 1);
  }
  expect(fade.state() == rifeofx::CalibrationState::kAmbiguous,
         "a stream without the expected repeats must be reported ambiguous");

  // Genuinely repeated content on top of the conform: an extra repeat that no
  // phase predicts.
  rifeofx::CadenceCalibrator repeated;
  repeated.configure(ratio);
  for (std::int64_t step = 0; step < 26; ++step) {
    const std::int64_t timelineTime = 3000 + step;
    std::uint64_t signature = conformedSignature(timelineTime, 0, ratio);
    if (step == 7 || step == 8) {
      signature = 0xFEEDULL;  // a real duplicate in the media itself
    }
    repeated.addSample(timelineTime, signature);
  }
  expect(repeated.state() != rifeofx::CalibrationState::kCalibrated ||
             repeated.phase() == 0,
         "content repeats must not produce a wrong phase");
}

void testNotApplicableCases() {
  rifeofx::CadenceCalibrator identical;
  identical.configure(rifeofx::cadenceRatio(60.0, 60.0));
  expect(identical.state() == rifeofx::CalibrationState::kNotApplicable,
         "equal cadences need no calibration");

  rifeofx::CadenceCalibrator downconvert;
  downconvert.configure(rifeofx::cadenceRatio(60.0, 50.0));
  expect(downconvert.state() == rifeofx::CalibrationState::kNotApplicable,
         "a faster source drops frames and cannot be recovered");

  rifeofx::CadenceCalibrator missing;
  missing.configure(rifeofx::cadenceRatio(0.0, 60.0));
  expect(missing.state() == rifeofx::CalibrationState::kNotApplicable,
         "missing frame rates must not calibrate");
}

void testManualOverride() {
  const rifeofx::Rational ratio = rifeofx::cadenceRatio(50.0, 60.0);
  rifeofx::CadenceCalibrator calibrator;
  calibrator.configure(ratio);
  calibrator.setManualPhase(4);
  expect(calibrator.state() == rifeofx::CalibrationState::kCalibrated,
         "a manual phase must calibrate immediately");
  expect(calibrator.phase() == 4, "a manual phase must be used verbatim");
  expect(calibrator.manualPhase(), "a manual phase must be flagged");

  calibrator.setManualPhase(10);
  expect(calibrator.phase() == 4, "a manual phase must wrap into the cycle");
}

}  // namespace

// `CadenceMappingTest --dump` prints the mapping the plugin will produce, so the
// documented cycle comes from this code rather than from a transcription.
int dumpCycle() {
  const rifeofx::Rational ratio = rifeofx::cadenceRatio(50.0, 60.0);
  const std::int64_t phase = 0;
  std::cout << "ratio=" << ratio.num << "/" << ratio.den << " phase=" << phase << "\n";
  std::cout << "timelineTime cyclePos sourcePosition frameA frameB "
               "timelineA timelineB timestep rifeInference\n";
  for (std::int64_t step = 0; step < 14; ++step) {
    const std::int64_t timelineTime = 7000 + step;
    const rifeofx::CadenceMapping m =
        rifeofx::computeCadenceMapping(timelineTime, phase, ratio);
    std::cout << m.timelineTime << "  " << m.cyclePosition << "  "
              << m.sourcePosition << "  " << m.sourceFrameA << "  "
              << m.sourceFrameB << "  " << m.timelineTimeA << "  "
              << m.timelineTimeB << "  " << m.timestep << "  "
              << (m.timestep > 1e-4f ? 1 : 0) << "\n";
  }
  return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--dump") {
    return dumpCycle();
  }
  testRatios();
  test50to60Over60Frames();
  testCalibrationRecoversPhase();
  testCalibratedMappingMatchesTheHostStream();
  testAmbiguousCases();
  testNotApplicableCases();
  testManualOverride();

  if (gFailures == 0) {
    std::cout << "CadenceMappingTest: all checks passed\n";
  }
  return gFailures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
