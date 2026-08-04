#include "engine/wasapi_capture.h"

#include "common/log.h"
#include "engine/device_manager.h"

#include <format>

namespace audiolens {

WasapiCapture::~WasapiCapture() { close(); }

bool WasapiCapture::open(const std::wstring& deviceId, bool loopback, std::uint32_t bufferMs,
                         std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    close();

    ComPtr<IMMDevice> device = openDevice(deviceId);
    if (!device) {
        return fail("キャプチャ用デバイスを開けませんでした");
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
        return fail("キャプチャ側の音声フォーマットが未対応です");
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (loopback) {
        flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    const REFERENCE_TIME duration = static_cast<REFERENCE_TIME>(bufferMs) * 10000;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, mix.get(), nullptr);
    if (FAILED(hr)) {
        return fail(std::format("キャプチャクライアントの初期化に失敗: {}", hresultToString(hr)));
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

    hr = client_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        return fail(std::format("IAudioCaptureClient の取得に失敗: {}", hresultToString(hr)));
    }

    AL_INFO("キャプチャ: {} / バッファ {} フレーム ({:.1f} ms){}", format_.describe(), bufferFrames_,
            1000.0 * bufferFrames_ / format_.sampleRate, loopback ? " / ループバック" : "");
    return true;
}

void WasapiCapture::close() {
    stop();
    capture_.Reset();
    client_.Reset();
    event_.reset();
    format_ = {};
    bufferFrames_ = 0;
}

bool WasapiCapture::start(std::string* error) {
    if (!client_) {
        if (error != nullptr) {
            *error = "キャプチャが初期化されていません";
        }
        return false;
    }
    if (running_) {
        return true;
    }
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        if (error != nullptr) {
            *error = std::format("キャプチャの開始に失敗: {}", hresultToString(hr));
        }
        return false;
    }
    running_ = true;
    return true;
}

void WasapiCapture::stop() {
    if (client_ && running_) {
        client_->Stop();
        client_->Reset();
        running_ = false;
    }
}

double WasapiCapture::streamLatencyMs() const {
    if (!client_) {
        return 0.0;
    }
    REFERENCE_TIME latency = 0;
    if (FAILED(client_->GetStreamLatency(&latency))) {
        return 0.0;
    }
    return static_cast<double>(latency) / 10000.0;
}

bool WasapiCapture::nextPacketSize(std::uint32_t* frames, HRESULT* hr) const {
    *frames = 0;
    if (!capture_) {
        if (hr != nullptr) {
            *hr = AUDCLNT_E_NOT_INITIALIZED;
        }
        return false;
    }
    const HRESULT result = capture_->GetNextPacketSize(frames);
    if (hr != nullptr) {
        *hr = result;
    }
    return SUCCEEDED(result);
}

HRESULT WasapiCapture::acquirePacket(const void** data, std::uint32_t* frames, DWORD* flags) {
    BYTE* buffer = nullptr;
    UINT64 devicePosition = 0;
    UINT64 qpcPosition = 0;
    const HRESULT hr = capture_->GetBuffer(&buffer, frames, flags, &devicePosition, &qpcPosition);
    if (SUCCEEDED(hr)) {
        *data = (*flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : buffer;
    }
    return hr;
}

HRESULT WasapiCapture::releasePacket(std::uint32_t frames) { return capture_->ReleaseBuffer(frames); }

}  // namespace audiolens
