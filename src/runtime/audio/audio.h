#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace audio {

constexpr uint32_t kRate = 48000;
// the output's channel layouts: stereo, or 5.1 in WAVE / SDL order (front L, front R, centre, LFE, back L, back R),
// which is the order of the console's speaker mix bins 0-5
constexpr uint32_t kStereo = 2, kSurround = 6;

struct Device {
    std::string id, name;  // id is what SelectDevice and the audio_device setting take
};

// the outputs there are; any thread
std::vector<Device> Devices();
// moves the output to the device with this id, the system's default when "" or gone; any thread, before or after
// StartOutput (which starts on app::Get().audio_device). It also reopens the output after app::Get().speakers changes
void SelectDevice(const std::string& id);

// how many channels the output takes for the speakers setting ("stereo", "5.1", or "auto": 5.1 when the device has
// at least six channels, so a stereo device or headphones keep the stereo mix) and the device's own channel count
inline uint32_t ChannelsFor(const std::string& speakers, uint32_t device_channels) {
    if (speakers == "stereo") return kStereo;
    if (speakers == "5.1") return kSurround;
    return device_channels >= kSurround ? kSurround : kStereo;
}
// the channels the output mixes now (kStereo until it opens); the title's speaker config follows it
inline std::atomic<uint32_t> g_channels{kStereo};

// the host's own sounds, played over the game; any thread
enum class Cue { Select, Move };  // a tab or setting changed; the cursor moved
void Play(Cue cue);
// adds the playing ones to interleaved frames of this many channels, on the front pair; the output's callback calls it
// after the game's mix
void MixCues(float* out, uint32_t frames, uint32_t channels);

// a title's own gain over the game's sound, asked once per mixed block on the audio thread (Conker: its cutscenes,
// louder by a setting); null or 1 is none. Set before the title boots
inline float (*g_title_gain)() = nullptr;

// the platform output pulls interleaved float frames of kStereo or kSurround channels from render
void StartOutput(std::function<void(float* out, uint32_t frames, uint32_t channels)> render);

}  // namespace audio
