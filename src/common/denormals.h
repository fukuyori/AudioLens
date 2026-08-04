#pragma once

#include <xmmintrin.h>
#include <pmmintrin.h>

namespace audiolens {

/// Flushes denormal floats to zero for the lifetime of the object.
///
/// Filter and envelope state decays exponentially toward zero, so after a
/// passage of silence it lands in the denormal range. Denormal arithmetic is
/// handled by microcode on x86 and costs orders of magnitude more than normal
/// arithmetic, which is enough to blow a real-time audio deadline. Every thread
/// that runs the DSP chain must scope one of these.
class ScopedNoDenormals {
public:
    ScopedNoDenormals() noexcept
        : savedFlushZero_(_MM_GET_FLUSH_ZERO_MODE()),
          savedDenormalsZero_(_MM_GET_DENORMALS_ZERO_MODE()) {
        _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
        _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    }

    ~ScopedNoDenormals() {
        _MM_SET_FLUSH_ZERO_MODE(savedFlushZero_);
        _MM_SET_DENORMALS_ZERO_MODE(savedDenormalsZero_);
    }

    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
    unsigned int savedFlushZero_;
    unsigned int savedDenormalsZero_;
};

}  // namespace audiolens
