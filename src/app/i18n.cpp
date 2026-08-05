#include "app/i18n.h"

#include <QApplication>
#include <QLocale>
#include <QTranslator>

#include <memory>

namespace audiolens::app {
namespace {

/// Owned here rather than by the caller so that switching language can retire
/// the previous catalogue without the caller having to keep hold of it.
std::unique_ptr<QTranslator> g_translator;

}  // namespace

QString languageToString(Language language) {
    switch (language) {
        case Language::English: return QStringLiteral("en");
        case Language::Japanese: return QStringLiteral("ja");
        case Language::System: break;
    }
    return QStringLiteral("system");
}

Language languageFromString(const QString& text) {
    if (text == QStringLiteral("en")) return Language::English;
    if (text == QStringLiteral("ja")) return Language::Japanese;
    return Language::System;
}

Language installTranslator(QApplication& app, Language language) {
    if (language == Language::System) {
        // Only the language matters, not the region: a machine set to ja-JP and
        // one set to Japanese with some other region want the same interface.
        language = QLocale::system().language() == QLocale::Japanese ? Language::Japanese
                                                                    : Language::English;
    }

    if (g_translator) {
        QApplication::removeTranslator(g_translator.get());
        g_translator.reset();
    }

    // English needs no catalogue: it is what the source strings are written in.
    // Returning early here also means a missing or broken .qm can never leave
    // the app in a half-translated state — it is either Japanese or the
    // original English, never a mixture.
    if (language == Language::English) {
        return language;
    }

    auto translator = std::make_unique<QTranslator>();
    if (translator->load(QStringLiteral(":/i18n/AudioLens_ja"))) {
        QApplication::installTranslator(translator.get());
        g_translator = std::move(translator);
        return Language::Japanese;
    }

    // The catalogue is compiled into the executable, so failing to load it means
    // the build is wrong rather than the installation. Fall back to English
    // rather than showing untranslated markers.
    return Language::English;
    (void)app;
}

QString presetName(const Preset& preset) {
    const QString id = QString::fromStdString(preset.id);

    // Listed one by one, and deliberately not generated from the preset table.
    // lupdate reads source text, not runtime data: a loop over the presets
    // calling tr() on each name would compile and would extract nothing.
    if (id == QStringLiteral("standard")) return QApplication::translate("Preset", "Standard");
    if (id == QStringLiteral("conversation")) return QApplication::translate("Preset", "Conversation");
    if (id == QStringLiteral("lecture")) return QApplication::translate("Preset", "Lecture");
    if (id == QStringLiteral("movie")) return QApplication::translate("Preset", "Film");
    if (id == QStringLiteral("night")) return QApplication::translate("Preset", "Late night");
    if (id == QStringLiteral("game")) return QApplication::translate("Preset", "Game");
    if (id == QStringLiteral("rock")) return QApplication::translate("Preset", "Rock");
    if (id == QStringLiteral("jazz")) return QApplication::translate("Preset", "Jazz");
    if (id == QStringLiteral("classical")) return QApplication::translate("Preset", "Classical");
    if (id == QStringLiteral("ambient")) return QApplication::translate("Preset", "Ambient");

    // A preset the user made and named. Left exactly as typed.
    return QString::fromStdString(preset.name);
}

QString presetDescription(const Preset& preset) {
    const QString id = QString::fromStdString(preset.id);

    if (id == QStringLiteral("standard"))
        return QApplication::translate(
            "Preset", "No correction. The sound passes through untouched, with no processing and no latency.");
    if (id == QStringLiteral("conversation"))
        return QApplication::translate(
            "Preset", "Brings voices in calls and meetings forward and evens out loudness firmly.");
    if (id == QStringLiteral("lecture"))
        return QApplication::translate(
            "Preset", "Moderate clarity with the harsh top held back, so long listening stays comfortable.");
    if (id == QStringLiteral("movie"))
        return QApplication::translate(
            "Preset", "Narrows the gap between dialogue and effects while keeping the weight of the bass.");
    if (id == QStringLiteral("night"))
        return QApplication::translate(
            "Preset", "Levels as far as it goes so quiet playback stays audible, and cuts the bass hard.");
    if (id == QStringLiteral("game"))
        return QApplication::translate(
            "Preset", "Cuts the lows and lifts the upper mids, making quiet sounds and their direction clear.");
    if (id == QStringLiteral("rock"))
        return QApplication::translate(
            "Preset", "Lifts vocals out of distorted guitars and tidies up a saturated low end.");
    if (id == QStringLiteral("jazz"))
        return QApplication::translate(
            "Preset", "Keeps every instrument of a small group distinct and rounds off only the edgy top.");
    if (id == QStringLiteral("classical"))
        return QApplication::translate(
            "Preset", "Preserves the dynamics and the hall, and touches as little as possible.");
    if (id == QStringLiteral("ambient"))
        return QApplication::translate(
            "Preset", "Raises fine detail at the top a little and leaves the width and the swells alone.");

    return QString::fromStdString(preset.description);
}

}  // namespace audiolens::app
