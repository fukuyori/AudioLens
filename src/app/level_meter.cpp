#include "app/level_meter.h"

#include <QLinearGradient>
#include <QPainter>
#include <QPalette>

#include <algorithm>
#include <cmath>

namespace audiolens::app {
namespace {

constexpr float kFloorDb = -60.0f;
constexpr float kMaxReductionDb = 24.0f;

float linearToDb(float linear) {
    return linear > 1e-6f ? 20.0f * std::log10(linear) : -120.0f;
}

/// Maps dBFS onto 0..1 across the bar, giving the top of the range more room
/// than a straight linear mapping would.
double positionForDb(float db) {
    const double normalized = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
    return std::pow(normalized, 0.7);
}

void drawTrack(QPainter& painter, const QRect& rect, const QPalette& palette) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.color(QPalette::Base).darker(115));
    painter.drawRoundedRect(rect, 3, 3);
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
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect track = rect().adjusted(0, 0, -1, -1);
    drawTrack(painter, track, palette());

    const double fraction = positionForDb(levelDb_);
    if (fraction <= 0.0) {
        return;
    }

    QRect filled = track;
    filled.setWidth(static_cast<int>(std::lround(track.width() * fraction)));

    // Green through amber to red, so "too loud" is legible without a number.
    QLinearGradient gradient(track.topLeft(), track.topRight());
    gradient.setColorAt(0.0, QColor(0x3f, 0xa7, 0x5f));
    gradient.setColorAt(positionForDb(-12.0f), QColor(0x5f, 0xb8, 0x54));
    gradient.setColorAt(positionForDb(-3.0f), QColor(0xd6, 0xa5, 0x31));
    gradient.setColorAt(1.0, QColor(0xd0, 0x45, 0x3c));

    painter.setBrush(gradient);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(filled, 3, 3);
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
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect track = rect().adjusted(0, 0, -1, -1);
    drawTrack(painter, track, palette());

    const double fraction =
        std::clamp(static_cast<double>(-reductionDb_) / kMaxReductionDb, 0.0, 1.0);
    if (fraction <= 0.0) {
        return;
    }

    const int width = static_cast<int>(std::lround(track.width() * fraction));
    QRect filled = track;
    filled.setLeft(track.right() - width);

    painter.setBrush(QColor(0x4a, 0x8d, 0xc4));
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(filled, 3, 3);
}

}  // namespace audiolens::app
