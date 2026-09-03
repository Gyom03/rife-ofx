#include "CadenceMapping.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <sstream>
#include <vector>

namespace rifeofx {

namespace {

// A cycle longer than this cannot be calibrated from a plausible run of
// consecutive renders, and a ratio that reduces to such a cycle is far more
// likely to be a misread frame rate than a real cadence.
constexpr std::int64_t kMaximumPeriodTimeline = 30;
// Two full cycles plus one sample: enough for the repeat pattern to be
// periodic rather than coincidental.
constexpr std::int64_t kCalibrationCycles = 2;
constexpr std::size_t kMaximumSamples = 128;

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
  const std::int64_t quotient = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? quotient - 1 : quotient;
}

std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
  const std::int64_t quotient = a / b;
  return (a % b != 0 && ((a < 0) == (b < 0))) ? quotient + 1 : quotient;
}

Rational reduce(std::int64_t num, std::int64_t den) {
  if (den == 0) {
    return {0, 1};
  }
  if (den < 0) {
    num = -num;
    den = -den;
  }
  const std::int64_t divisor = std::gcd(num < 0 ? -num : num, den);
  if (divisor > 1) {
    num /= divisor;
    den /= divisor;
  }
  return {num, den};
}

// A frame rate as an exact fraction. Integers stay integers; broadcast rates
// are recognised as k*1000/1001 even when the user typed the rounded 23.976,
// so the ratio does not drift. Anything else falls back to a bounded continued
// fraction.
Rational rationalizeFrameRate(double frameRate) {
  if (!(frameRate > 0.0) || !std::isfinite(frameRate)) {
    return {0, 1};
  }

  const double nearestInteger = std::round(frameRate);
  if (std::abs(frameRate - nearestInteger) < 1e-6) {
    return {static_cast<std::int64_t>(nearestInteger), 1};
  }

  const double ntscWhole = frameRate * 1.001;
  const double nearestNtsc = std::round(ntscWhole);
  if (nearestNtsc > 0.0 && std::abs(ntscWhole - nearestNtsc) < 1e-3) {
    return reduce(static_cast<std::int64_t>(nearestNtsc) * 1000, 1001);
  }

  // Continued fraction, bounded so a nonsense rate cannot produce a huge cycle.
  constexpr std::int64_t kMaximumDenominator = 1001;
  std::int64_t previousNum = 1, previousDen = 0;
  std::int64_t num = static_cast<std::int64_t>(std::floor(frameRate));
  std::int64_t den = 1;
  double remainder = frameRate - std::floor(frameRate);
  for (int iteration = 0; iteration < 16 && remainder > 1e-9; ++iteration) {
    const double inverse = 1.0 / remainder;
    const std::int64_t whole = static_cast<std::int64_t>(std::floor(inverse));
    const std::int64_t nextNum = whole * num + previousNum;
    const std::int64_t nextDen = whole * den + previousDen;
    if (nextDen > kMaximumDenominator) {
      break;
    }
    previousNum = num;
    previousDen = den;
    num = nextNum;
    den = nextDen;
    remainder = inverse - std::floor(inverse);
  }
  return reduce(num, den);
}

// Original frame index carried by cycle offset u.
std::int64_t sourceFrameAtOffset(std::int64_t offset, const Rational& ratio) {
  return floorDiv(ratio.num * offset, ratio.den);
}

// True when this timeline time repeats the image of the previous one, i.e. the
// host emitted a duplicate here.
bool predictsRepeat(std::int64_t offset, const Rational& ratio) {
  return sourceFrameAtOffset(offset, ratio) ==
         sourceFrameAtOffset(offset - 1, ratio);
}

}  // namespace

const char* describeCalibrationState(CalibrationState state) {
  switch (state) {
    case CalibrationState::kPending:
      return "pending";
    case CalibrationState::kCalibrated:
      return "calibrated";
    case CalibrationState::kAmbiguous:
      return "ambiguous";
    case CalibrationState::kNotApplicable:
      return "notApplicable";
  }
  return "unknown";
}

Rational cadenceRatio(double sourceFrameRate, double timelineFrameRate) {
  const Rational source = rationalizeFrameRate(sourceFrameRate);
  const Rational timeline = rationalizeFrameRate(timelineFrameRate);
  if (source.num <= 0 || timeline.num <= 0) {
    return {0, 1};
  }
  return reduce(source.num * timeline.den, source.den * timeline.num);
}

void CadenceCalibrator::configure(const Rational& ratio) {
  // A repeated call with the same ratio must not discard a measurement, but the
  // first call has to run even when the ratio happens to equal the default.
  if (configured_ && ratio.num == ratio_.num && ratio.den == ratio_.den) {
    return;
  }
  configured_ = true;
  ratio_ = ratio;
  samples_.clear();
  manualPhase_ = false;
  phase_ = 0;

  if (ratio_.num <= 0 || ratio_.den <= 0) {
    state_ = CalibrationState::kNotApplicable;
    reason_ = "frame rates unavailable";
    return;
  }
  if (ratio_.num == ratio_.den) {
    // Same cadence: the host inserted nothing, every timeline frame is an
    // original, and no interpolation is called for.
    state_ = CalibrationState::kNotApplicable;
    reason_ = "source and timeline cadence are identical";
    return;
  }
  if (ratio_.num > ratio_.den) {
    // The host dropped frames rather than repeating them. The missing
    // originals are simply not present in the input, so there is nothing to
    // recover and nothing to calibrate.
    state_ = CalibrationState::kNotApplicable;
    reason_ = "source cadence is faster than the timeline; frames are dropped, not repeated";
    return;
  }
  if (ratio_.den > kMaximumPeriodTimeline) {
    state_ = CalibrationState::kAmbiguous;
    reason_ = "cycle of " + std::to_string(ratio_.den) +
              " timeline frames is too long to calibrate";
    return;
  }
  state_ = CalibrationState::kPending;
  reason_ = "collecting consecutive samples";
}

void CadenceCalibrator::setManualPhase(std::int64_t phase) {
  if (ratio_.den <= 0) {
    return;
  }
  const std::int64_t wrapped = ((phase % ratio_.den) + ratio_.den) % ratio_.den;
  manualPhase_ = true;
  phase_ = wrapped;
  state_ = CalibrationState::kCalibrated;
  reason_ = "manual phase override";
  samples_.clear();
}

void CadenceCalibrator::reset() {
  samples_.clear();
  manualPhase_ = false;
  phase_ = 0;
  if (ratio_.num > 0 && ratio_.den > 0 && ratio_.num < ratio_.den &&
      ratio_.den <= kMaximumPeriodTimeline) {
    state_ = CalibrationState::kPending;
    reason_ = "collecting consecutive samples";
  }
}

std::int64_t CadenceCalibrator::samplesRequired() const {
  if (ratio_.den <= 0) {
    return 0;
  }
  return kCalibrationCycles * ratio_.den + 1;
}

bool CadenceCalibrator::addSample(std::int64_t timelineTime,
                                  std::uint64_t signature) {
  if (state_ != CalibrationState::kPending) {
    return false;
  }
  const auto existing = samples_.find(timelineTime);
  if (existing != samples_.end() && existing->second == signature) {
    return false;
  }
  samples_[timelineTime] = signature;
  while (samples_.size() > kMaximumSamples) {
    samples_.erase(samples_.begin());
  }
  return evaluate();
}

bool CadenceCalibrator::evaluate() {
  const std::int64_t required = samplesRequired();

  // Longest run of consecutive timeline times, preferring the most recent so a
  // window spoiled by real repeated content eventually falls out.
  std::vector<std::pair<std::int64_t, std::uint64_t>> best;
  std::vector<std::pair<std::int64_t, std::uint64_t>> run;
  for (const auto& sample : samples_) {
    if (!run.empty() && sample.first != run.back().first + 1) {
      if (run.size() >= best.size()) {
        best = run;
      }
      run.clear();
    }
    run.push_back(sample);
  }
  if (run.size() >= best.size()) {
    best = run;
  }
  if (static_cast<std::int64_t>(best.size()) < required) {
    reason_ = "collecting consecutive samples (" + std::to_string(best.size()) +
              "/" + std::to_string(required) + ")";
    return false;
  }

  // A run with a single distinct image carries no cadence information at all.
  std::vector<std::uint64_t> distinct;
  for (const auto& sample : best) {
    if (std::find(distinct.begin(), distinct.end(), sample.second) ==
        distinct.end()) {
      distinct.push_back(sample.second);
    }
  }
  if (distinct.size() < 2) {
    state_ = CalibrationState::kAmbiguous;
    reason_ = "every sampled frame is identical (static image, black, or frozen clip)";
    return true;
  }

  // Observed repeats, then the phases that predict exactly this pattern.
  std::vector<std::int64_t> candidates;
  for (std::int64_t phase = 0; phase < ratio_.den; ++phase) {
    bool matches = true;
    for (std::size_t index = 1; index < best.size(); ++index) {
      const bool observed = best[index].second == best[index - 1].second;
      const std::int64_t offset = best[index].first - phase;
      if (observed != predictsRepeat(offset, ratio_)) {
        matches = false;
        break;
      }
    }
    if (matches) {
      candidates.push_back(phase);
    }
  }

  std::size_t observedRepeats = 0;
  for (std::size_t index = 1; index < best.size(); ++index) {
    if (best[index].second == best[index - 1].second) {
      ++observedRepeats;
    }
  }

  std::ostringstream detail;
  detail << "run=" << best.size() << " from=" << best.front().first
         << " distinct=" << distinct.size() << " repeats=" << observedRepeats;

  if (candidates.size() == 1) {
    phase_ = candidates.front();
    state_ = CalibrationState::kCalibrated;
    reason_ = detail.str();
    return true;
  }
  if (candidates.empty()) {
    // Either the host is not repeating on the expected cycle, or the media
    // itself contains repeated or cross-faded frames. Both are reasons not to
    // guess a phase.
    state_ = CalibrationState::kAmbiguous;
    reason_ = "no phase fits the observed repeats; " + detail.str();
    return true;
  }
  state_ = CalibrationState::kAmbiguous;
  reason_ = std::to_string(candidates.size()) + " phases fit equally well; " +
            detail.str();
  return true;
}

std::int64_t timelineTimeForSourceFrame(std::int64_t sourceFrame,
                                        std::int64_t phase,
                                        const Rational& ratio) {
  if (ratio.num <= 0) {
    return phase + sourceFrame;
  }
  // sourceFrameAtOffset(u) == k holds for den*k/num <= u < den*(k+1)/num, so
  // the first timeline frame of the block is the ceiling of the lower bound.
  // Taking the first one is what guarantees a host-duplicated frame is never
  // handed to RIFE as one of the two inputs.
  return phase + ceilDiv(ratio.den * sourceFrame, ratio.num);
}

CadenceMapping computeCadenceMapping(std::int64_t timelineTime,
                                     std::int64_t phase, const Rational& ratio) {
  CadenceMapping mapping;
  mapping.timelineTime = timelineTime;
  mapping.phase = phase;
  mapping.periodSource = ratio.num;
  mapping.periodTimeline = ratio.den;
  if (ratio.num <= 0 || ratio.den <= 0) {
    mapping.timelineTimeA = timelineTime;
    mapping.timelineTimeB = timelineTime;
    return mapping;
  }

  const std::int64_t offset = timelineTime - phase;
  mapping.cyclePosition = ((offset % ratio.den) + ratio.den) % ratio.den;

  // Exact rational position: numerator over den, so the timestep is a ratio of
  // integers and never accumulates floating point error.
  const std::int64_t numerator = ratio.num * offset;
  const std::int64_t frame = floorDiv(numerator, ratio.den);
  const std::int64_t remainder = numerator - frame * ratio.den;

  mapping.sourceFrameA = frame;
  mapping.sourceFrameB = frame + 1;
  mapping.sourcePosition =
      static_cast<double>(numerator) / static_cast<double>(ratio.den);
  mapping.timestep =
      static_cast<float>(static_cast<double>(remainder) /
                         static_cast<double>(ratio.den));
  mapping.timelineTimeA = timelineTimeForSourceFrame(frame, phase, ratio);
  mapping.timelineTimeB = timelineTimeForSourceFrame(frame + 1, phase, ratio);
  mapping.valid = true;
  return mapping;
}

}  // namespace rifeofx
