add_executable(audiolens_procloop main.cpp)

target_link_libraries(audiolens_procloop PRIVATE
    audiolens::common
    mmdevapi        # ActivateAudioInterfaceAsync
    ole32
)

# Process loopback is declared behind NTDDI_WIN10_FE (build 20348). The shared
# options only ask for _WIN32_WINNT=0x0A00, which resolves to plain NTDDI_WIN10
# and compiles the declarations out entirely. Raising it here rather than
# globally keeps the rest of the codebase building against the floor set by
# requirement N-06 (Windows 10 2004).
target_compile_definitions(audiolens_procloop PRIVATE
    NTDDI_VERSION=0x0A00000B    # NTDDI_WIN10_FE
)
