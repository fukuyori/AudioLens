#pragma once

#include "core/preset.h"

#include <QJsonObject>

namespace audiolens::app {

/// Bumped whenever the on-disk shape changes in a way a reader has to know
/// about. Files carrying a newer version than this build understands are
/// refused rather than silently misread.
inline constexpr int kPresetSchemaVersion = 1;

QJsonObject presetToJson(const Preset& preset);

/// Returns false if the object is not a preset this build can read. `error`
/// gets a message suitable for showing to the user.
bool presetFromJson(const QJsonObject& json, Preset* out, QString* error);

}  // namespace audiolens::app
