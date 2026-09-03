#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace rifeofx {

// The host resamples a clip whose media cadence differs from the timeline
// before the effect sees it. For 50 into 60 that means six timeline frames
// carry five original images, one of them repeated. The originals are all still
// there, so the job is to address them: which timeline time carries original
// frame k, and where between two originals a given output frame falls.
//
// Everything here is integer arithmetic on a reduced rational ratio. The only
// measured quantity is the phase of the repeat, and it is measured once.

struct Rational {
  std::int64_t num = 0;
  std::int64_t den = 1;
};

// Reduced sourceFPS / timelineFPS. num is the number of original frames per
// cycle, den the number of timeline frames per cycle: 50/60 gives 5/6,
// 23.976/59.94 gives 2/5. Broadcast rates are recognised as k*1000/1001 so the
// ratio stays exact instead of accumulating floating point residue.
Rational cadenceRatio(double sourceFrameRate, double timelineFrameRate);

enum class CalibrationState {
  kPending = 0,      // still collecting consecutive samples
  kCalibrated = 1,   // phase known, mapping is pure arithmetic from here on
  kAmbiguous = 2,    // the observed pattern fits no single phase; do not guess
  kNotApplicable = 3 // no host resampling to undo (equal rates, or downconversion)
};

const char* describeCalibrationState(CalibrationState state);

// Recovers the phase of the host conform from a run of consecutive rendered
// frames. It only ever compares image signatures, and only until it succeeds.
class CadenceCalibrator {
 public:
  // Reconfiguring with a different ratio discards any measurement.
  void configure(const Rational& ratio);

  // One rendered timeline time and the signature of the image the host returned
  // for it. Returns true when the state or the phase changed, which is the cue
  // to log. Samples are ignored once calibrated.
  bool addSample(std::int64_t timelineTime, std::uint64_t signature);

  void setManualPhase(std::int64_t phase);
  void reset();

  CalibrationState state() const { return state_; }
  std::int64_t phase() const { return phase_; }
  const Rational& ratio() const { return ratio_; }
  bool manualPhase() const { return manualPhase_; }
  const std::string& reason() const { return reason_; }
  std::size_t sampleCount() const { return samples_.size(); }
  // Consecutive samples still needed before a verdict is possible.
  std::int64_t samplesRequired() const;

 private:
  bool evaluate();

  Rational ratio_;
  bool configured_ = false;
  CalibrationState state_ = CalibrationState::kPending;
  std::int64_t phase_ = 0;
  bool manualPhase_ = false;
  std::string reason_ = "not configured";
  std::map<std::int64_t, std::uint64_t> samples_;
};

struct CadenceMapping {
  std::int64_t timelineTime = 0;
  std::int64_t phase = 0;
  std::int64_t cyclePosition = 0;   // (timelineTime - phase) mod periodTimeline
  std::int64_t periodSource = 1;    // original frames per cycle
  std::int64_t periodTimeline = 1;  // timeline frames per cycle
  double sourcePosition = 0.0;      // exact rational, in original frames
  std::int64_t sourceFrameA = 0;
  std::int64_t sourceFrameB = 0;
  // Timeline times that carry those two original images. Always the first
  // timeline frame of each block, so a repeated frame is never fetched.
  std::int64_t timelineTimeA = 0;
  std::int64_t timelineTimeB = 0;
  float timestep = 0.0f;
  bool valid = false;
};

CadenceMapping computeCadenceMapping(std::int64_t timelineTime, std::int64_t phase,
                                     const Rational& ratio);

// First timeline time carrying original frame index k. Exposed for tests.
std::int64_t timelineTimeForSourceFrame(std::int64_t sourceFrame,
                                        std::int64_t phase, const Rational& ratio);

}  // namespace rifeofx
