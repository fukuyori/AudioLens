add_library(audiolens_dsp STATIC
    auto_gain.cpp
    auto_gain.h
    biquad.cpp
    biquad.h
    compressor.cpp
    compressor.h
    dsp_chain.cpp
    dsp_chain.h
    k_weighting.cpp
    k_weighting.h
    limiter.cpp
    limiter.h
    parameters.h
    resampler.cpp
    resampler.h
)
add_library(audiolens::dsp ALIAS audiolens_dsp)

# DspChain implements IAudioProcessor, which the engine declares. The filters
# themselves stay free of Windows headers so they can be tested offline.
target_link_libraries(audiolens_dsp PUBLIC audiolens::engine)
