#pragma once

// Single place that pulls in <Windows.h> with the macro hygiene the rest of the
// codebase assumes (no min/max macros, no winsock1 leakage).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
