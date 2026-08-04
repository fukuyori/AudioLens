#pragma once

#include "dsp/biquad.h"

namespace audiolens::dsp {

/// The two stages of the ITU-R BS.1770 K-weighting curve.
///
/// K-weighting is what makes a loudness figure agree with what a listener
/// actually hears: a shelf standing in for the head's response above about
/// 1.7 kHz, and a highpass discounting the very low frequencies the ear is
/// insensitive to.
///
/// BS.1770-4 publishes coefficients only for 48 kHz. The constants below are
/// the analog prototype the published values were derived from, so the same
/// filters can be built for any sample rate; at 48 kHz they reproduce the
/// published numbers exactly.
///
/// These live here, next to Biquad, because both the offline measurement in
/// `analysis/` and the real-time levelling in `AutoGain` need them, and the two
/// must not be allowed to drift apart — a levelling stage aiming at a target
/// measured with a different curve would miss by however much the curves
/// differ.

BiquadCoeffs designKWeightingShelf(double sampleRate);
BiquadCoeffs designKWeightingHighpass(double sampleRate);

}  // namespace audiolens::dsp
