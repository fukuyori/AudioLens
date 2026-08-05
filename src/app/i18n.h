#pragma once

#include "core/preset.h"

#include <QString>

class QApplication;

namespace audiolens::app {

/// Which language the interface is in.
///
/// `System` is the default and means "whatever Windows is set to", which is
/// right for an app that is normally never configured. The two explicit values
/// exist because the system language and the language you want to read software
/// in are not always the same.
enum class Language {
    System,
    English,
    Japanese,
};

/// The stored form, e.g. "system", "en", "ja". Kept as text rather than an
/// integer so the settings file stays readable (requirement N-09) and so that
/// adding a language later does not renumber the existing ones.
QString languageToString(Language language);
Language languageFromString(const QString& text);

/// Loads the catalogue for `language` and installs it. Safe to call again when
/// the user changes the setting; the previous translator is removed first.
///
/// Returns the language actually in force, which is never `System`: that value
/// is resolved against the system locale here so that the rest of the app never
/// has to ask what it meant.
Language installTranslator(QApplication& app, Language language);

/// The display name of a preset, translated for the built-in ones.
///
/// User presets are returned untouched. Their names were typed by the user, and
/// running those through a translation table would either do nothing or, worse,
/// occasionally match a built-in name and rewrite it.
///
/// The built-in names are listed here rather than in `core` on purpose. Nothing
/// in `core` links against Qt — the engine, the DSP and the tests all build
/// without it — and adding a translation call there to save this table would
/// cost that. `core` keeps the English text, which is what the CLI tools print.
QString presetName(const Preset& preset);
QString presetDescription(const Preset& preset);

}  // namespace audiolens::app
