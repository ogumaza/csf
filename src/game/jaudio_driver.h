// SPDX-License-Identifier: MIT

// What game mode's JAudio drivers share (game/jaudio1_driver.h and game/jaudio2_driver.h). A JAudio game runs its
// sound's frame process in its frames, 30 a second. A driver waits for the game's sound start-up, and for the sound's
// wave groups, a game frame at a time. Each wait has a limit.

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include "game/game_driver.h"

namespace csf::game
{

class JAudioDriver : public GameDriver
{
protected:
    using GameDriver::GameDriver;

    // From now on, each of the game's frames runs the frame process. Until then, the game's frames only pass.
    void StartFrameProcess();

    // Runs the game's frames until `done()` holds. `each_frame`, if given, runs before each of them. Throws
    // std::runtime_error with the message `failure` if `done()` doesn't hold after kWaitFrames frames.
    void WaitUntil(const std::function<bool()>& done, const std::string& failure,
                   const std::function<void()>& each_frame = {});

    // Loads the sound's wave groups `waves`, each the wave bank or system in the high half and the group in the low
    // half. `load(wave)` starts the load of each group for which `loaded(wave)` doesn't hold. The game's frames then
    // run until `loaded(wave)` holds for every group, one group at a time. Each group may take kWaitFrames frames.
    // Throws std::runtime_error if one takes more.
    void LoadWaves(std::span<const uint32_t> waves, const std::function<bool(uint32_t)>& loaded,
                   const std::function<void(uint32_t)>& load);

private:
    // The game's frames per second, and the frames that a wait may take before the driver gives up.
    static constexpr uint64_t kGameFramesPerSecond = 30;
    static constexpr int kWaitFrames = 600;

    // Runs AX's frames up to the game's next frame.
    void GameFrame();

    // Runs the game's frame process once in each of the game's frames that has come, once StartFrameProcess has been
    // called.
    void RunMainLoop() final;

    // The game's sound frame process: what each of the game's frames runs for its sound.
    virtual void FrameProcess() = 0;

    bool ready_ = false;       // the frame process runs
    uint64_t game_frames_ = 0; // the game's frames so far
};

} // namespace csf::game
