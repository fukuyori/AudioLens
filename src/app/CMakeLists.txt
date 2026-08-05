qt_add_executable(AudioLens WIN32
    audio_controller.cpp
    audio_controller.h
    default_device_guard.cpp
    default_device_guard.h
    i18n.cpp
    i18n.h
    level_meter.cpp
    level_meter.h
    main.cpp
    main_window.cpp
    main_window.h
    preset_json.cpp
    preset_json.h
    settings_store.cpp
    settings_store.h
)

set_target_properties(AudioLens PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME AudioLens
)

target_link_libraries(AudioLens PRIVATE
    audiolens::core
    Qt6::Widgets
)

# Japanese, compiled into the executable rather than shipped beside it.
#
# A .qm file next to the exe is one more thing an installer can leave out and
# one more thing a user can delete, and the failure mode — an app that silently
# reverts to English — is the kind nobody reports as a bug. Embedded in the
# binary it either works or the build is broken, which is a better place for the
# question to be settled.
#
# Source strings are English (see i18n.h for why), so only ja needs a catalogue.
#
# Do NOT regenerate the .ts with lupdate without checking the context names
# afterwards. These sources open their namespace as `namespace audiolens::app {`
# — the C++17 nested form — and lupdate does not resolve it: it emits
# `<name>MainWindow</name>` while moc, and therefore tr() at run time, uses
# `audiolens::app::MainWindow`. The names do not meet, no string is found, and
# the app runs in English while reporting that it loaded the catalogue.
#
# The .ts in this directory has the qualified names filled in by hand. lupdate
# will helpfully undo that. Contexts named with an explicit string in
# QCoreApplication::translate (Preset, PresetJson, SettingsStore,
# DefaultDeviceGuard) are unaffected, which is why those translated correctly
# while the two tr() classes did not.
if(TARGET Qt6::lupdate)
    qt_add_translations(AudioLens
        TS_FILES "${CMAKE_CURRENT_SOURCE_DIR}/translations/AudioLens_ja.ts"
        RESOURCE_PREFIX "/i18n"
    )
else()
    message(WARNING "Qt LinguistTools が見つかりません。日本語 UI は無効になります。")
endif()
