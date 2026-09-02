#include "mic_capture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <vector>

namespace speechdoc {

MicCapture::MicCapture(QObject* parent) : QObject(parent) {}

MicCapture::~MicCapture() {
    stop();
}

bool MicCapture::start() {
    if (running_.load())
        return true;
    // captureLoop() may have ended after a device error.  Its std::thread is
    // still joinable even though running_ is false, and assigning over it
    // would call std::terminate().  Reap it before allowing a restart.
    if (thread_.joinable())
        thread_.join();
    running_.store(true);
    thread_ = std::thread([this] { captureLoop(); });
    return true;
}

void MicCapture::stop() {
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
}

void MicCapture::captureLoop() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsCom = SUCCEEDED(initialized) || initialized == S_FALSE;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* format = nullptr;

    auto releaseAll = [&] {
        if (format)
            CoTaskMemFree(format);
        if (capture)
            capture->Release();
        if (client)
            client->Release();
        if (device)
            device->Release();
        if (enumerator)
            enumerator->Release();
        if (ownsCom)
            CoUninitialize();
    };
    auto fail = [&](const QString& message) {
        running_.store(false);
        emit error(message);
        releaseAll();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))
        || FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole,
                                                       &device))
        || FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                   nullptr,
                                   reinterpret_cast<void**>(&client)))
        || FAILED(client->GetMixFormat(&format))) {
        fail(QStringLiteral("打开默认麦克风失败（WASAPI）"));
        return;
    }

    constexpr REFERENCE_TIME bufferDuration = 2'000'000;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0,
                                  bufferDuration, 0, format, nullptr))
        || FAILED(client->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&capture)))
        || FAILED(client->Start())) {
        fail(QStringLiteral("启动麦克风采集失败（WASAPI）"));
        return;
    }

    const int sampleRate = format->nSamplesPerSec > 0
        ? static_cast<int>(format->nSamplesPerSec) : 48000;
    const int channels = (std::max)(1, static_cast<int>(format->nChannels));
    const int bits = format->wBitsPerSample;
    bool floatSamples = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool pcmSamples = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        floatSamples = IsEqualGUID(extended->SubFormat,
                                   KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        pcmSamples = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    if ((!floatSamples && !pcmSamples)
        || (floatSamples && bits != 32)
        || (pcmSamples && bits != 8 && bits != 16 && bits != 24 && bits != 32)) {
        client->Stop();
        fail(QStringLiteral("默认麦克风格式不受支持（%1 位）").arg(bits));
        return;
    }
    const int bytesPerSample = bits / 8;
    std::vector<std::int16_t> buffered;
    QString runtimeError;

    while (running_.load()) {
        UINT32 packetFrames = 0;
        if (FAILED(capture->GetNextPacketSize(&packetFrames))) {
            runtimeError = QStringLiteral("读取麦克风数据包失败（WASAPI）");
            break;
        }
        if (packetFrames == 0) {
            Sleep(10);
            continue;
        }

        BYTE* bytes = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&bytes, &frames, &flags,
                                      nullptr, nullptr))) {
            runtimeError = QStringLiteral("获取麦克风缓冲区失败（WASAPI）");
            break;
        }

        for (UINT32 frame = 0; frame < frames; ++frame) {
            float mono = 0.0f;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                double sum = 0.0;
                for (int channel = 0; channel < channels; ++channel) {
                    const BYTE* sample = bytes
                        + (static_cast<size_t>(frame) * channels + channel)
                            * bytesPerSample;
                    if (floatSamples) {
                        float value = 0.0f;
                        std::memcpy(&value, sample, sizeof(value));
                        sum += value;
                    } else if (bits == 8) {
                        sum += (static_cast<int>(*sample) - 128) / 128.0;
                    } else if (bits == 16) {
                        std::int16_t value = 0;
                        std::memcpy(&value, sample, sizeof(value));
                        sum += value / 32768.0;
                    } else if (bits == 24) {
                        std::int32_t value = static_cast<std::int32_t>(sample[0])
                            | (static_cast<std::int32_t>(sample[1]) << 8)
                            | (static_cast<std::int32_t>(sample[2]) << 16);
                        if (value & 0x00800000) value |= ~0x00ffffff;
                        sum += value / 8388608.0;
                    } else {
                        std::int32_t value = 0;
                        std::memcpy(&value, sample, sizeof(value));
                        sum += value / 2147483648.0;
                    }
                }
                mono = static_cast<float>(sum / channels);
            }
            if (!std::isfinite(mono)) mono = 0.0f;
            mono = (std::max)(-1.0f, (std::min)(1.0f, mono));
            const int scaled = (std::max)(-32768, (std::min)(
                32767, static_cast<int>(mono * 32767.0f)));
            buffered.push_back(static_cast<std::int16_t>(scaled));
        }
        capture->ReleaseBuffer(frames);

        const int chunkFrames = (std::max)(1, sampleRate / 10);
        while (static_cast<int>(buffered.size()) >= chunkFrames) {
            emit pcmReady(
                QByteArray(reinterpret_cast<const char*>(buffered.data()),
                           chunkFrames * static_cast<int>(sizeof(std::int16_t))),
                sampleRate);
            buffered.erase(buffered.begin(), buffered.begin() + chunkFrames);
        }
    }

    client->Stop();
    running_.store(false);
    releaseAll();
    if (!runtimeError.isEmpty())
        emit error(runtimeError);
}

}  // namespace speechdoc
