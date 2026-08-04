add_executable(audiolens_process main.cpp)

target_link_libraries(audiolens_process PRIVATE
    audiolens::analysis
    audiolens::audiofile
    audiolens::core
)
