#include "mic_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <vector>

namespace voiceengine {

MicCapture::MicCapture(QObject* parent) : QObject(parent) {}

MicCapture::~MicCapture() { stop(); }

bool MicCapture::start() {
    if (m_running.load()) {
        return true;
    }
    m_running.store(true);
    m_thread = std::thread([this] { loop(); });
    return true;
}

void MicCapture::stop() {
    m_running.store(false);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void MicCapture::loop() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool did_co = SUCCEEDED(hr) || hr == S_FALSE;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* fmt = nullptr;

    auto fail = [&](const char* msg) {
        emit error(QString::fromUtf8(msg));
        if (fmt) {
            CoTaskMemFree(fmt);
        }
        if (cap) {
            cap->Release();
        }
        if (client) {
            client->Release();
        }
        if (dev) {
            dev->Release();
        }
        if (en) {
            en->Release();
        }
        if (did_co) {
            CoUninitialize();
        }
        m_running.store(false);
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(&client))) ||
        FAILED(client->GetMixFormat(&fmt))) {
        fail("无法打开默认麦克风（WASAPI）");
        return;
    }

    const REFERENCE_TIME dur = 2000000;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, fmt, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap))) ||
        FAILED(client->Start())) {
        fail("WASAPI 采集启动失败");
        return;
    }
    m_sr = fmt->nSamplesPerSec > 0 ? static_cast<int>(fmt->nSamplesPerSec) : 48000;
    const int ch = fmt->nChannels > 0 ? fmt->nChannels : 1;
    const int bits = fmt->wBitsPerSample;
    const bool ieee = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE);

    std::vector<int16_t> acc;
    while (m_running.load()) {
        UINT32 pkt = 0;
        if (FAILED(cap->GetNextPacketSize(&pkt))) {
            break;
        }
        if (pkt == 0) {
            Sleep(10);
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) {
            break;
        }
        for (UINT32 i = 0; i < frames; ++i) {
            float mono = 0;
            if (ieee && bits == 32) {
                const float* f = reinterpret_cast<const float*>(data);
                double s = 0;
                for (int c = 0; c < ch; ++c) {
                    s += f[i * ch + c];
                }
                mono = static_cast<float>(s / ch);
            } else if (bits == 16) {
                const int16_t* s16 = reinterpret_cast<const int16_t*>(data);
                int accv = 0;
                for (int c = 0; c < ch; ++c) {
                    accv += s16[i * ch + c];
                }
                mono = static_cast<float>(accv / ch) / 32768.0f;
            }
            int v = static_cast<int>(mono * 32767.0f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            acc.push_back(static_cast<int16_t>(v));
        }
        cap->ReleaseBuffer(frames);
        const int window = (std::max)(1, m_sr / 10);
        while (static_cast<int>(acc.size()) >= window) {
            QByteArray chunk(reinterpret_cast<const char*>(acc.data()), window * 2);
            emit pcmReady(chunk, m_sr);
            acc.erase(acc.begin(), acc.begin() + window);
        }
    }
    client->Stop();
    if (fmt) {
        CoTaskMemFree(fmt);
    }
    if (cap) cap->Release();
    if (client) client->Release();
    if (dev) dev->Release();
    if (en) en->Release();
    if (did_co) CoUninitialize();
}

}  // namespace voiceengine
