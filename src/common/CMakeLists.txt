add_library(audiolens_common STATIC
    com.cpp
    com.h
    denormals.h
    log.cpp
    log.h
    windows_lean.h
)
add_library(audiolens::common ALIAS audiolens_common)

target_include_directories(audiolens_common PUBLIC "${CMAKE_SOURCE_DIR}/src")

target_link_libraries(audiolens_common
    PUBLIC audiolens::options
    PRIVATE ole32
)
