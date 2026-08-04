#pragma once

#include "common/com.h"
#include "engine/audio_format.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstdint>
#include <string>

namespace audiolens {

/// Event-driven WASAPI capture in shared mode.
///
/// With `loopback` set, the endpoint is a *render* device and the client
/// receives whatever the OS mixer is sending to it. That is how AudioLens taps
/// system audio: the virtual device is the default output, and this reads its
/// mix.
class WasapiCapture {
public:
    WasapiCapture() = default;
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    bool open(const std::wstring& deviceId, bool loopback, std::uint32_t bufferMs,
              std::string* error);
    void close();

    bool start(std::string* error);
    void stop();

    const StreamFormat& format() const noexcept { return format_; }
    std::uint32_t bufferFrames() const noexcept { return bufferFrames_; }
    HANDLE eventHandle() const noexcept { return event_.get(); }

    /// Latency the endpoint itself adds, in milliseconds.
    double streamLatencyMs() const;

    /// Number of frames in the next queued packet, or 0 if none is ready.
    /// Returns false and sets `hr` on failure (notably AUDCLNT_E_DEVICE_INVALIDATED).
    bool nextPacketSize(std::uint32_t* frames, HRESULT* hr) const;

    /// Acquires the next packet. `data` is null when the packet is silent.
    /// Every successful call must be paired with releasePacket().
    HRESULT acquirePacket(const void** data, std::uint32_t* frames, DWORD* flags);
    HRESULT releasePacket(std::uint32_t frames);

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    UniqueHandle event_;
    StreamFormat format_;
    std::uint32_t bufferFrames_ = 0;
    bool running_ = false;
};

}  // namespace audiolens
