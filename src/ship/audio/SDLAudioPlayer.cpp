#include "ship/audio/SDLAudioPlayer.h"
#include <spdlog/spdlog.h>

#ifdef __PS4__
// PS4: SDL has no audio driver there, so this "SDL" player talks to sceAudioOut directly.
// The main audio port only accepts 48 kHz, the game mixes at a lower rate: a small feeder thread
// resamples (linear interpolation) and hands 256 frame blocks to the system.

#include <orbis/AudioOut.h>
#include "ship/port/ps4/Ps4Platform.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace Ship {
namespace {
constexpr uint32_t kOutputRate = 48000;
constexpr uint32_t kGranularity = 256;
constexpr int32_t kSystemUserId = 0xFF;

std::mutex sQueueMutex;
// Diagnostics, read and reset by Ps4Audio_TakeStats().
std::atomic<uint32_t> sDroppedBuffers{ 0 };
std::atomic<uint32_t> sStarvedBlocks{ 0 };
std::vector<int16_t> sQueue; // interleaved stereo frames at the game's sample rate
size_t sQueueRead = 0;       // index of the first unread sample in sQueue
std::thread sFeeder;
std::atomic<bool> sRunning{ false };
int32_t sPort = -1;
uint32_t sSourceRate = 32000;

void FeederThread() {
    int16_t block[kGranularity * 2];
    double position = 0.0; // read position in source frames, relative to sQueueRead

    while (sRunning) {
        {
            std::lock_guard<std::mutex> lock(sQueueMutex);
            const double step = (double)sSourceRate / (double)kOutputRate;
            const size_t available = (sQueue.size() - sQueueRead) / 2;
            const int16_t* frames = sQueue.data() + sQueueRead;

            bool starved = false;
            for (uint32_t i = 0; i < kGranularity; i++) {
                const size_t index = (size_t)position;
                if (index + 1 < available) {
                    const double fraction = position - (double)index;
                    const int16_t* a = frames + index * 2;
                    block[i * 2 + 0] = (int16_t)((double)a[0] + ((double)a[2] - (double)a[0]) * fraction);
                    block[i * 2 + 1] = (int16_t)((double)a[1] + ((double)a[3] - (double)a[1]) * fraction);
                    position += step;
                } else if (index < available) {
                    block[i * 2 + 0] = frames[index * 2 + 0];
                    block[i * 2 + 1] = frames[index * 2 + 1];
                    position += step;
                } else {
                    // Underrun: play silence until the game catches up.
                    block[i * 2 + 0] = 0;
                    block[i * 2 + 1] = 0;
                    starved = true;
                }
            }
            if (starved) {
                sStarvedBlocks.fetch_add(1);
            }

            size_t consumed = (size_t)position;
            if (consumed > available) {
                consumed = available;
            }
            sQueueRead += consumed * 2;
            position -= (double)consumed;
            if (position >= 1.0) {
                position = 0.0;
            }

            if (sQueueRead >= 8192 && sQueueRead * 2 >= sQueue.size()) {
                sQueue.erase(sQueue.begin(), sQueue.begin() + (std::ptrdiff_t)sQueueRead);
                sQueueRead = 0;
            }
        }

        // Blocks until the previous block has been played.
        sceAudioOutOutput(sPort, block);
    }
}
} // namespace

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct PS4 audio player");
    DoClose();
}

void SDLAudioPlayer::DoClose() {
    if (sRunning.exchange(false)) {
        if (sFeeder.joinable()) {
            sFeeder.join();
        }
    }
    if (sPort >= 0) {
        sceAudioOutClose(sPort);
        sPort = -1;
    }
    {
        std::lock_guard<std::mutex> lock(sQueueMutex);
        sQueue.clear();
        sQueueRead = 0;
    }
    mDevice = 0;
}

} // namespace Ship

extern "C" void Ps4Audio_TakeStats(uint32_t* droppedBuffers, uint32_t* starvedBlocks) {
    *droppedBuffers = Ship::sDroppedBuffers.exchange(0);
    *starvedBlocks = Ship::sStarvedBlocks.exchange(0);
}

namespace Ship {
bool SDLAudioPlayer::DoInit() {
    mNumChannels = this->GetNumOutputChannels();
    sSourceRate = (uint32_t)this->GetSampleRate();

    Ship::Ps4::LoadSystemModules();
    // Returns an "already initialized" error when called twice, which is fine.
    sceAudioOutInit();

    sPort = sceAudioOutOpen(kSystemUserId, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGranularity, kOutputRate,
                            ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (sPort < 0) {
        SPDLOG_ERROR("[PS4] sceAudioOutOpen failed: 0x{:08X}", (uint32_t)sPort);
        sPort = -1;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(sQueueMutex);
        sQueue.clear();
        sQueue.reserve(1 << 16);
        sQueueRead = 0;
    }

    sRunning = true;
    sFeeder = std::thread(FeederThread);
    mDevice = 1;

    SPDLOG_INFO("[PS4] audio initialized: {} channels, {} Hz resampled to {} Hz", mNumChannels, sSourceRate,
                kOutputRate);
    return true;
}

int SDLAudioPlayer::Buffered() {
    std::lock_guard<std::mutex> lock(sQueueMutex);
    // The system output has no buffer of its own beyond one 256 frame block, so a game tick that
    // runs a little late (20 Hz games refill once every 50 ms) used to drain the queue and cut
    // the sound. Under-report by a fixed cushion: the game's buffering logic then keeps that many
    // extra frames queued.
    constexpr int kCushionFrames = 3200; // 100 ms at 32 kHz
    const int queued = (int)((sQueue.size() - sQueueRead) / 2);
    return queued > kCushionFrames ? queued - kCushionFrames : 0;
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    if (sPort < 0 || mNumChannels < 2) {
        return;
    }

    const int16_t* samples = reinterpret_cast<const int16_t*>(buf);
    const size_t frames = len / (sizeof(int16_t) * (size_t)mNumChannels);

    std::lock_guard<std::mutex> lock(sQueueMutex);
    // Room for the 100 ms cushion plus a few game frames of audio (BenPort adds an update when low).
    if ((sQueue.size() - sQueueRead) / 2 >= 12000) {
        // Don't fill the audio buffer too much in case this happens
        sDroppedBuffers.fetch_add(1);
        return;
    }

    if (mNumChannels == 2) {
        sQueue.insert(sQueue.end(), samples, samples + frames * 2);
    } else {
        // Surround was requested but the port is stereo: keep the front pair.
        for (size_t i = 0; i < frames; i++) {
            sQueue.push_back(samples[i * (size_t)mNumChannels + 0]);
            sQueue.push_back(samples[i * (size_t)mNumChannels + 1]);
        }
    }
}
} // namespace Ship

#else

namespace Ship {

SDLAudioPlayer::~SDLAudioPlayer() {
    SPDLOG_TRACE("destruct SDL audio player");
    DoClose();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void SDLAudioPlayer::DoClose() {
    if (mDevice != 0) {
        // Pause playback first
        SDL_PauseAudioDevice(mDevice, 1);
        // Clear any queued audio to prevent glitches when reopening
        SDL_ClearQueuedAudio(mDevice);
        SDL_CloseAudioDevice(mDevice);
        mDevice = 0;
    }
}

bool SDLAudioPlayer::DoInit() {
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        SPDLOG_ERROR("SDL init error: {}", SDL_GetError());
        return false;
    }

    // Always open with the correct number of output channels
    mNumChannels = this->GetNumOutputChannels();

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = this->GetSampleRate();
    want.format = AUDIO_S16SYS;
    want.channels = mNumChannels;
    want.samples = this->GetSampleLength();
    want.callback = NULL;

    mDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (mDevice == 0) {
        SPDLOG_ERROR("SDL_OpenAudio error: {}", SDL_GetError());
        return false;
    }

    SPDLOG_INFO("SDL Audio initialized: {} channels, {} Hz", mNumChannels, this->GetSampleRate());

    SDL_PauseAudioDevice(mDevice, 0);
    return true;
}

int SDLAudioPlayer::Buffered() {
    return SDL_GetQueuedAudioSize(mDevice) / (sizeof(int16_t) * mNumChannels);
}

void SDLAudioPlayer::DoPlay(const uint8_t* buf, size_t len) {
    if (Buffered() < 6000) {
        // Don't fill the audio buffer too much in case this happens
        SDL_QueueAudio(mDevice, buf, len);
    }
}
} // namespace Ship
#endif // __PS4__
