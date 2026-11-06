// mixer output through SDL's audio, everywhere but Windows (audio/xaudio2.cpp)
#include <SDL3/SDL.h>

#include <mutex>
#include <vector>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace audio {

namespace {

// what the device asks for, rendered 10 ms at a time as on XAudio2, in the channels the stream was opened with
void Pull(void* user, SDL_AudioStream* stream, int additional, int) {
    auto& render = *static_cast<std::function<void(float*, uint32_t, uint32_t)>*>(user);
    constexpr uint32_t kFrames = kRate / 100;
    const uint32_t ch = g_channels.load();
    static std::vector<float> buf;
    buf.resize(size_t(kFrames) * ch);
    const int bytes = int(kFrames * ch * sizeof(float));
    for (int left = additional; left > 0; left -= bytes) {
        render(buf.data(), kFrames, ch);
        SDL_PutAudioStreamData(stream, buf.data(), bytes);
    }
}

std::function<void(float*, uint32_t, uint32_t)> g_render;  // the device's callback reads it for the run
std::mutex g_lock;                                 // the stream and what it plays on
SDL_AudioStream* g_stream = nullptr;
bool g_started = false;

SDL_AudioDeviceID Find(const std::string& name) {
    SDL_AudioDeviceID found = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    int n = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) {
        for (int i = 0; i < n; i++)
            if (name == SDL_GetAudioDeviceName(ids[i])) found = ids[i];
        SDL_free(ids);
    }
    return found;
}

// (re)opens the output on the named device; g_lock held
void Open(const std::string& name) {
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
    const SDL_AudioDeviceID dev = name.empty() ? SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK : Find(name);
    // the device's own channels pick stereo or 5.1 (SDL's 6 channel order is the mix bins' order); SDL converts
    // what we give to what the device takes, so a forced 5.1 on a stereo device is folded down by SDL
    SDL_AudioSpec have{};
    int device_channels = 2;
    if (SDL_GetAudioDeviceFormat(dev, &have, nullptr)) device_channels = have.channels;
    const uint32_t ch = ChannelsFor(app::Get().speakers, uint32_t(device_channels));
    g_channels = ch;
    SDL_AudioSpec spec{SDL_AUDIO_F32, int(ch), int(kRate)};
    g_stream = SDL_OpenAudioDeviceStream(dev, &spec, Pull, &g_render);
    if (!g_stream) {
        LOGE("audio: no output device, no sound: %s", SDL_GetError());
        return;
    }
    LOGI("audio: output %u channels (device %d, speakers %s)", ch, device_channels, app::Get().speakers.c_str());
    SDL_ResumeAudioStreamDevice(g_stream);
}

}  // namespace

std::vector<Device> Devices() {
    std::vector<Device> out;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return out;
    int n = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n)) {
        for (int i = 0; i < n; i++)
            if (const char* name = SDL_GetAudioDeviceName(ids[i])) out.push_back({name, name});
        SDL_free(ids);
    }
    return out;
}

void SelectDevice(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_started) Open(id);
}

void StartOutput(std::function<void(float*, uint32_t, uint32_t)> render) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("audio: SDL audio unavailable, no sound: %s", SDL_GetError());
        return;
    }
    std::lock_guard<std::mutex> lock(g_lock);
    g_render = std::move(render);
    g_started = true;
    Open(app::Get().audio_device);
    LOGI("audio: SDL %s", SDL_GetCurrentAudioDriver());
}

}  // namespace audio
