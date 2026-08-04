qt_add_executable(AudioLens WIN32
    audio_controller.cpp
    audio_controller.h
    default_device_guard.cpp
    default_device_guard.h
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
