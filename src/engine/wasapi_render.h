#pragma once

#include "common/com.h"
#include "engine/audio_format.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cstdint>
#include <string>

namespace audiolens {

/// Event-driven WASAPI playback in shared mode.
class WasapiRender {
public:
    WasapiRender() = default;
    ~WasapiRender();

    WasapiRender(const WasapiRender&) = delete;
    WasapiRender& operator=(const WasapiRender&) = delete;

    bool open(const std::wstring& deviceId, std::uint32_t bufferMs, std::string* error);
    void close();

    bool start(std::string* error);
    void stop();

    const StreamFormat& format() const noexcept { return format_; }
    std::uint32_t bufferFrames() const noexcept { return bufferFrames_; }
    HANDLE eventHandle() const noexcept { return event_.get(); }

    double streamLatencyMs() const;

    /// Frames already queued in the endpoint buffer and not yet played.
    bool padding(std::uint32_t* frames, HRESULT* hr) const;

    HRESULT acquireBuffer(std::uint32_t frames, void** data);
    HRESULT releaseBuffer(std::uint32_t frames, DWORD flags);

private:
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    UniqueHandle event_;
    StreamFormat format_;
    std::uint32_t bufferFrames_ = 0;
    bool running_ = false;
};

}  // namespace audiolens
