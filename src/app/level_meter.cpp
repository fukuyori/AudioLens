#include "app/level_meter.h"

#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace audiolens::app {
namespace {

constexpr float kFloorDb = -60.0f;
constexpr float kMaxReductionDb = 24.0f;

/// Number of lamps across the bar, and the gap between them in pixels.
///
/// Twenty is enough that the bar still reads as a level rather than a row of
/// blocks, and few enough that each lamp is wide enough to carry a colour at
/// this height. The gap is what makes it legible as segments at all: without it
/// the neighbouring hues blend and the boundaries disappear, which was the
/// failing of the smooth gradient this replaces.
constexpr int kSegments = 20;
constexpr int kSegmentGap = 2;

float linearToDb(float linear) {
    return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f;
}

/// Maps dBFS onto 0..1 across the bar, giving the top of the range more room
/// than a straight linear mapping would.
double positionForDb(float db) {
    const double normalized = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
    return std::pow(normalized, 0.7);
}

/// The inverse, so a lamp can be asked what level it stands for.
float dbForPosition(double position) {
    const double normalized = std::pow(std::clamp(position, 0.0, 1.0), 1.0 / 0.7);
    return static_cast<float>(kFloorDb + normalized * -kFloorDb);
}

/// The colour of the lamp that stands for `db`.
///
/// Fixed to the scale, not to the current level. That is the whole point of the
/// rewrite: a lamp's colour never changes, so "the bar reached orange" is a
/// statement about where the signal got to, which is how every meter on every
/// piece of audio hardware is read. Colouring the whole bar from the level
/// instead — which is what this did briefly — encodes the same number twice,
/// once as length and once as hue, and makes the entire bar pulse with every
/// syllable for information the length had already given.
///
/// The stops are levels a listener cares about rather than round numbers.
QColor colourForDb(float db) {
    struct Stop {
        float db;
        QColor colour;
    };
    static const Stop stops[] = {
        {-60.0f, QColor(0x24, 0xc4, 0x4a)},  // green
        {-24.0f, QColor(0x5a, 0xd0, 0x2e)},  // still green, a shade warmer
        {-15.0f, QColor(0xc8, 0xd8, 0x1c)},  // yellow-green
        {-9.0f, QColor(0xf2, 0xd0, 0x18)},   // yellow
        {-4.5f, QColor(0xf2, 0x8f, 0x18)},   // orange
        {0.0f, QColor(0xe8, 0x30, 0x22)},    // red
    };
    constexpr int count = static_cast<int>(std::size(stops));

    if (db <= stops[0].db) {
        return stops[0].colour;
    }
    for (int i = 1; i < count; ++i) {
        if (db > stops[i].db) {
            continue;
        }
        const float span = stops[i].db - stops[i - 1].db;
        const qreal t = span > 0.0f ? (db - stops[i - 1].db) / span : 0.0;
        // Interpolated in HSV so green→yellow→red passes through the hues
        // between them; in RGB the midpoint of green and red is a muddy brown.
        return QColor::fromHsvF(
            stops[i - 1].colour.hueF() + t * (stops[i].colour.hueF() - stops[i - 1].colour.hueF()),
            stops[i - 1].colour.saturationF() +
                t * (stops[i].colour.saturationF() - stops[i - 1].colour.saturationF()),
            stops[i - 1].colour.valueF() +
                t * (stops[i].colour.valueF() - stops[i - 1].colour.valueF()));
    }
    return stops[count - 1].colour;
}

void drawTrack(QPainter& painter, const QRect& rect, const QPalette& palette) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.color(QPalette::Base).darker(150));
    painter.drawRoundedRect(rect, 2, 2);
}

/// The rectangle of lamp `index`, laid out so rounding never leaves a ragged
/// right-hand edge: each boundary is computed from the full width rather than
/// by accumulating a per-lamp width.
QRect segmentRect(const QRect& track, int index) {
    const double step = static_cast<double>(track.width()) / kSegments;
    const int left = track.left() + static_cast<int>(std::lround(index * step));
    const int right = track.left() + static_cast<int>(std::lround((index + 1) * step)) - kSegmentGap;
    return {left, track.top(), std::max(1, right - left), track.height()};
}

/// Draws the lamps. `litFraction` is 0..1 of the track; lamps beyond it are
/// shown as a dark trace of their own colour rather than blank.
///
/// The unlit trace is deliberate. It puts the scale on screen at all times, so
/// the user can see where the red end is *before* reaching it — which is the
/// affordance a meter is supposed to provide and the one the old smooth bar,
/// being invisible until filled, never did.
void drawSegments(QPainter& painter, const QRect& track, double litFraction) {
    for (int i = 0; i < kSegments; ++i) {
        const double centre = (i + 0.5) / kSegments;
        const QColor colour = colourForDb(dbForPosition(centre));
        // Lit once the level has passed the lamp's centre, so a lamp is either
        // clearly on or clearly off and never half-shaded.
        const bool lit = litFraction >= centre;

        QColor fill = colour;
        if (!lit) {
            fill = QColor::fromHsvF(colour.hueF(), colour.saturationF() * 0.75,
                                    colour.valueF() * 0.16);
        }
        painter.setBrush(fill);
        painter.drawRect(segmentRect(track, i));
    }
}

}  // namespace

LevelMeter::LevelMeter(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void LevelMeter::setLevel(float peak) {
    const float db = linearToDb(peak);
    if (std::fabs(db - levelDb_) < 0.2f) {
        return;  // Not worth a repaint.
    }
    levelDb_ = db;
    update();
}

QSize LevelMeter::sizeHint() const { return {180, 12}; }
QSize LevelMeter::minimumSizeHint() const { return {60, 8}; }

void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);

    const QRect track = rect().adjusted(0, 0, -1, -1);
    drawTrack(painter, track, palette());

    painter.setPen(Qt::NoPen);
    drawSegments(painter, track, positionForDb(levelDb_));
}

GainReductionMeter::GainReductionMeter(QWidget* parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void GainReductionMeter::setReduction(float reductionDb) {
    const float clamped = std::min(0.0f, reductionDb);
    if (std::fabs(clamped - reductionDb_) < 0.2f) {
        return;
    }
    reductionDb_ = clamped;
    update();
}

QSize GainReductionMeter::sizeHint() const { return {180, 12}; }
QSize GainReductionMeter::minimumSizeHint() const { return {60, 8}; }

void GainReductionMeter::paintEvent(QPaintEvent*) {
    QPainter painter(this);

    const QRect track = rect().adjusted(0, 0, -1, -1);
    drawTrack(painter, track, palette());

    const double fraction =
        std::clamp(static_cast<double>(-reductionDb_) / kMaxReductionDb, 0.0, 1.0);

    // Segmented like the level meters so the group reads as one instrument, but
    // one colour rather than a green-to-red ramp. The level bars going red mean
    // "running out of headroom"; this bar running a long way is the app doing
    // its job, and colouring it like a warning would say the opposite.
    painter.setPen(Qt::NoPen);
    const QColor lamp(0x4a, 0x8d, 0xc4);
    const QColor dark = QColor::fromHsvF(lamp.hueF(), lamp.saturationF() * 0.75,
                                         lamp.valueF() * 0.16);
    for (int i = 0; i < kSegments; ++i) {
        // Right to left: gain reduction is something being taken away.
        const double centre = (kSegments - i - 0.5) / kSegments;
        painter.setBrush(fraction >= centre ? lamp : dark);
        painter.drawRect(segmentRect(track, i));
    }
}

}  // namespace audiolens::app
