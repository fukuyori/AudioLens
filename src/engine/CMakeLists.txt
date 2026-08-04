add_library(audiolens_engine STATIC
    audio_engine.cpp
    audio_engine.h
    audio_format.cpp
    audio_format.h
    default_device.cpp
    default_device.h
    device_manager.cpp
    device_manager.h
    device_watcher.cpp
    device_watcher.h
    ring_buffer.h
    wasapi_capture.cpp
    wasapi_capture.h
    wasapi_render.cpp
    wasapi_render.h
)
add_library(audiolens::engine ALIAS audiolens_engine)

target_link_libraries(audiolens_engine
    PUBLIC audiolens::common
    PRIVATE
        ole32       # COM
        avrt        # MMCSS ("Pro Audio" thread scheduling)
        winmm       # WAVEFORMATEX helpers
)
