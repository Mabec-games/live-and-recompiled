// mixer output through XAudio2
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <xaudio2.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

std::mutex g_lock;
std::wstring g_want;              // the endpoint the output should be on, empty the default
std::atomic<uint32_t> g_change{0};  // counts SelectDevice calls, for the output thread

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
    return w;
}

std::string Narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 1 ? size_t(n - 1) : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

std::vector<Device> Devices() {
    std::vector<Device> out;
    const bool init = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* list = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&en))) &&
        SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
        UINT n = 0;
        list->GetCount(&n);
        // PKEY_Device_FriendlyName
        const PROPERTYKEY friendly = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        for (UINT i = 0; i < n; i++) {
            IMMDevice* dev = nullptr;
            if (FAILED(list->Item(i, &dev))) continue;
            LPWSTR id = nullptr;
            IPropertyStore* props = nullptr;
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(dev->GetId(&id))) {
                std::string label = Narrow(id);
                if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(friendly, &name)) &&
                    name.vt == VT_LPWSTR)
                    label = Narrow(name.pwszVal);
                out.push_back({Narrow(id), label});
                CoTaskMemFree(id);
            }
            PropVariantClear(&name);
            if (props) props->Release();
            dev->Release();
        }
    }
    if (list) list->Release();
    if (en) en->Release();
    if (init) CoUninitialize();
    return out;
}

void SelectDevice(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_want = Wide(id);
    }
    g_change++;
}

void StartOutput(std::function<void(float*, uint32_t, uint32_t)> render) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_want = Wide(app::Get().audio_device);
    }
    std::thread([render] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IXAudio2* xa = nullptr;
        IXAudio2MasteringVoice* master = nullptr;
        IXAudio2SourceVoice* voice = nullptr;
        uint32_t ch = kStereo;  // the source voice's channels, what render fills
        if (FAILED(XAudio2Create(&xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            LOGE("audio: XAudio2 unavailable, no sound");
            return;
        }
        // (re)creates the voices on the wanted device, the default when it is gone. The mastering voice takes the
        // endpoint's own layout (2, 5.1, 7.1, or what a virtual surround headset or Windows Sonic shows); the source
        // voice is stereo or 5.1 by the speakers setting and that layout, and XAudio2 maps it onto the endpoint
        // (5.1 onto a stereo endpoint is folded down by it)
        auto open = [&] {
            if (voice) voice->DestroyVoice();
            if (master) master->DestroyVoice();
            voice = nullptr, master = nullptr;
            std::wstring want;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                want = g_want;
            }
            if (want.empty() || FAILED(xa->CreateMasteringVoice(&master, XAUDIO2_DEFAULT_CHANNELS,
                                                                XAUDIO2_DEFAULT_SAMPLERATE, 0, want.c_str())))
                master = nullptr, xa->CreateMasteringVoice(&master);
            if (!master) return false;
            XAUDIO2_VOICE_DETAILS details{};
            master->GetVoiceDetails(&details);
            DWORD mask = 0;
            master->GetChannelMask(&mask);
            ch = ChannelsFor(app::Get().speakers, details.InputChannels);
            WAVEFORMATEXTENSIBLE wf{};
            wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wf.Format.nChannels = WORD(ch);
            wf.Format.nSamplesPerSec = kRate;
            wf.Format.wBitsPerSample = 32;
            wf.Format.nBlockAlign = WORD(ch * 4);
            wf.Format.nAvgBytesPerSec = kRate * ch * 4;
            wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wf.Samples.wValidBitsPerSample = 32;
            // SPEAKER_FRONT_LEFT | FRONT_RIGHT (| FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT)
            wf.dwChannelMask = ch == kSurround ? 0x3F : 0x3;
            // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            wf.SubFormat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
            if (FAILED(xa->CreateSourceVoice(&voice, &wf.Format))) {
                voice = nullptr;
                return false;
            }
            g_channels = ch;
            LOGI("audio: output %u channels (endpoint %u, mask %08lX, speakers %s)", ch, details.InputChannels,
                 static_cast<unsigned long>(mask), app::Get().speakers.c_str());
            voice->Start();
            return true;
        };
        uint32_t seen = g_change.load();
        if (!open()) {
            LOGE("audio: XAudio2 unavailable, no sound");
            return;
        }
        constexpr uint32_t kFrames = kRate / 100, kSlots = 4;
        std::vector<float> bufs(kSlots * kFrames * kSurround);  // room for the widest layout
        for (uint32_t n = 0;;) {
            if (seen != g_change.load()) {
                seen = g_change.load();
                if (!open()) {
                    LOGE("audio: no output device, no sound");
                    return;
                }
            }
            XAUDIO2_VOICE_STATE st;
            voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
            if (st.BuffersQueued >= 3) {
                Sleep(2);
                continue;
            }
            float* b = &bufs[(n % kSlots) * kFrames * kSurround];
            render(b, kFrames, ch);
            XAUDIO2_BUFFER xb{};
            xb.AudioBytes = kFrames * ch * 4;
            xb.pAudioData = reinterpret_cast<const BYTE*>(b);
            voice->SubmitSourceBuffer(&xb);
            n++;  // only a submitted buffer takes a slot, XAudio2 still reads the queued ones
        }
    }).detach();
}

}  // namespace audio
