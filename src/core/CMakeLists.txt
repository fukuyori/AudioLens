add_library(audiolens_core STATIC
    preset.cpp
    preset.h
)
add_library(audiolens::core ALIAS audiolens_core)

target_link_libraries(audiolens_core PUBLIC audiolens::dsp)
