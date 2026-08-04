add_library(audiolens_analysis STATIC
    loudness.cpp
    loudness.h
)
add_library(audiolens::analysis ALIAS audiolens_analysis)

target_link_libraries(audiolens_analysis PUBLIC audiolens::dsp)
