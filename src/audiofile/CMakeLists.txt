add_library(audiolens_audiofile STATIC
    wav.cpp
    wav.h
)
add_library(audiolens::audiofile ALIAS audiolens_audiofile)

target_include_directories(audiolens_audiofile PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(audiolens_audiofile PUBLIC audiolens::options)
