add_executable(audiolens_tone main.cpp)

target_link_libraries(audiolens_tone PRIVATE
    audiolens::audiofile
    audiolens::engine
)
