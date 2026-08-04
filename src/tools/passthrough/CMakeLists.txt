add_executable(audiolens_passthrough main.cpp)

target_link_libraries(audiolens_passthrough PRIVATE audiolens::core)
