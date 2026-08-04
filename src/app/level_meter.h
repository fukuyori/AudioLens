#pragma once

#include <QWidget>

namespace audiolens::app {

/// Horizontal level bar with a dBFS scale.
///
/// The scale is deliberately non-linear: -60 to 0 dBFS mapped linearly would
/// leave everything a listener actually cares about crammed into the last
/// fifth of the bar.
class LevelMeter : public QWidget {
    Q_OBJECT

public:
    explicit LevelMeter(QWidget* parent = nullptr);

    /// `peak` is linear, 1.0 being full scale.
    void setLevel(float peak);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float levelDb_ = -100.0f;
};

/// Downward bar showing how hard the compressor is working. Reads right to
/// left, because gain reduction is something being taken away.
class GainReductionMeter : public QWidget {
    Q_OBJECT

public:
    explicit GainReductionMeter(QWidget* parent = nullptr);

    /// `reductionDb` is zero or negative.
    void setReduction(float reductionDb);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    float reductionDb_ = 0.0f;
};

}  // namespace audiolens::app
