#include "engine/wasapi_render.h"

#include "common/log.h"
#include "engine/device_manager.h"

#include <format>

namespace audiolens {

WasapiRender::~WasapiRender() { close(); }

bool WasapiRender::open(const std::wstring& deviceId, std::uint32_t bufferMs, std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    close();

    ComPtr<IMMDevice> device = openDevice(deviceId);
    if (!device) {
        return fail("再生用デバイスを開けませんでした");
    }

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) {
        return fail(std::format("IAudioClient の取得に失敗: {}", hresultToString(hr)));
    }

    WAVEFORMATEX* rawMix = nullptr;
    hr = client_->GetMixFormat(&rawMix);
    if (FAILED(hr)) {
        return fail(std::format("ミックスフォーマットの取得に失敗: {}", hresultToString(hr)));
    }
    CoTaskMemPtr<WAVEFORMATEX> mix(rawMix);

    format_ = describeWaveFormat(mix.get());
    if (!format_.valid()) {
        return fail("再生側の音声フォーマットが未対応です");
    }

    const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                             0, mix.get(), nullptr);
    if (FAILED(hr)) {
        return fail(std::format("再生クライアントの初期化に失敗: {}", hresultToString(hr)));
    }

    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) {
        return fail(std::format("バッファサイズの取得に失敗: {}", hresultToString(hr)));
    }

    event_.reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event_) {
        return fail("イベントの作成に失敗しました");
    }
    hr = client_->SetEventHandle(event_.get());
    if (FAILED(hr)) {
        return fail(std::format("イベントハンドルの設定に失敗: {}", hresultToString(hr)));
    }

    hr = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) {
        return fail(std::format("IAudioRenderClient の取得に失敗: {}", hresultToString(hr)));
    }

    AL_INFO("再生: {} / バッファ {} フレーム ({:.1f} ms)", format_.describe(), bufferFrames_,
            1000.0 * bufferFrames_ / format_.sampleRate);
    return true;
}

void WasapiRender::close() {
    stop();
    render_.Reset();
    client_.Reset();
    event_.reset();
    format_ = {};
    bufferFrames_ = 0;
}

bool WasapiRender::start(std::string* error) {
    if (!client_) {
        if (error != nullptr) {
            *error = "再生が初期化されていません";
        }
        return false;
    }
    if (running_) {
        return true;
    }
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        if (error != nullptr) {
            *error = std::format("再生の開始に失敗: {}", hresultToString(hr));
        }
        return false;
    }
    running_ = true;
    return true;
}

void WasapiRender::stop() {
    if (client_ && running_) {
        client_->Stop();
        client_->Reset();
        running_ = false;
    }
}

double WasapiRender::streamLatencyMs() const {
    if (!client_) {
        return 0.0;
    }
    REFERENCE_TIME latency = 0;
    if (FAILED(client_->GetStreamLatency(&latency))) {
        return 0.0;
    }
    return static_cast<double>(latency) / 10000.0;
}

bool WasapiRender::padding(std::uint32_t* frames, HRESULT* hr) const {
    *frames = 0;
    if (!client_) {
        if (hr != nullptr) {
            *hr = AUDCLNT_E_NOT_INITIALIZED;
        }
        return false;
    }
    const HRESULT result = client_->GetCurrentPadding(frames);
    if (hr != nullptr) {
        *hr = result;
    }
    return SUCCEEDED(result);
}

HRESULT WasapiRender::acquireBuffer(std::uint32_t frames, void** data) {
    BYTE* buffer = nullptr;
    const HRESULT hr = render_->GetBuffer(frames, &buffer);
    if (SUCCEEDED(hr)) {
        *data = buffer;
    }
    return hr;
}

HRESULT WasapiRender::releaseBuffer(std::uint32_t frames, DWORD flags) {
    return render_->ReleaseBuffer(frames, flags);
}

}  // namespace audiolens
