add_executable(audiolens_tests
    test_support.cpp
    test_support.h
    test_audio_format.cpp
    test_auto_gain.cpp
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

add_test(NAME audiolens_tests COMMAND audiolens_tests)
