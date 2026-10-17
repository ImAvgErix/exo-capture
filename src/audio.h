// Exo Capture - sound: the game, Discord and the microphone as separate tracks plus their mix, as AAC.
// GPL-3.0-or-later.
#pragma once
#include "common.h"
#include "mux.h"

class AudioSource;
class AacEncoder;

class AudioCapture
{
public:
    struct Options
    {
        bool game = true;
        bool discord = false;
        bool mic = false;
        bool separate = true;
        DWORD gamePid = 0;    // 0: everything on the default output except Discord
        DWORD discordPid = 0; // Discord's root process, 0 when it is not running
    };

    AudioCapture(const Options& options, std::function<void(PacketPtr)> sink);
    ~AudioCapture();
    /// The AAC type of each audio track, track 1 first. Empty with no sound.
    std::vector<com_ptr<IMFMediaType>> Types() const;

private:
    void Mix();
    std::vector<std::unique_ptr<AudioSource>> m_sources;
    std::vector<std::unique_ptr<AacEncoder>> m_encoders;
    bool m_mixTrack = false;
    int64_t m_origin = 0;
    std::atomic<bool> m_stop{ false };
    std::thread m_thread;
};

/// The process id of the running Discord app (its top process), or 0.
DWORD FindDiscord();
