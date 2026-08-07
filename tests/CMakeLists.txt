add_executable(audiolens_tests
    test_support.cpp
    test_support.h
    test_audio_format.cpp
    test_auto_gain.cpp
    test_de_esser.cpp
    test_dsp_chain.cpp
    test_mid_side.cpp
    test_dsp_filters.cpp
    test_loudness.cpp
    test_resampler.cpp
    test_ring_buffer.cpp
    test_wav.cpp
)

target_include_directories(audiolens_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(audiolens_tests PRIVATE
    audiolens::analysis
    audiolens::audiofile
    audiolens::core
    audiolens::engine
)

# The command-line parser (requirement F-36) lives beside the GUI because it
# speaks QString, so it can only be tested where Qt was found. Guarded rather
# than made unconditional: a Qt-less checkout has to keep getting the rest of
# the suite, which is the whole reason the GUI is optional in the first place.
if(TARGET Qt6::Core)
    target_sources(audiolens_tests PRIVATE
        test_control_request.cpp
        "${CMAKE_SOURCE_DIR}/src/app/control_request.cpp"
    )
    target_link_libraries(audiolens_tests PRIVATE Qt6::Core)
endif()

add_test(NAME audiolens_tests COMMAND audiolens_tests)
