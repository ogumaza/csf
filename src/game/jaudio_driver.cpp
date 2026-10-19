// SPDX-License-Identifier: MIT

#include "game/jaudio_driver.h"

#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>

namespace csf::game
{

void JAudioDriver::StartFrameProcess()
{
    ready_ = true;
}

void JAudioDriver::WaitUntil(const std::function<bool()>& done, const std::string& failure,
                             const std::function<void()>& each_frame)
{
    for (int frame = 0; !done(); ++frame)
    {
        if (frame == kWaitFrames)
        {
            throw std::runtime_error(failure);
        }

        if (each_frame)
        {
            each_frame();
        }
        GameFrame();
    }
}

void JAudioDriver::LoadWaves(std::span<const uint32_t> waves, const std::function<bool(uint32_t)>& loaded,
                             const std::function<void(uint32_t)>& load)
{
    for (const uint32_t wave : waves)
    {
        if (!loaded(wave))
        {
            load(wave);
        }
    }

    for (const uint32_t wave : waves)
    {
        const std::string failure =
            "wave group " + std::to_string(wave >> 16) + "/" + std::to_string(wave & 0xffff) + " didn't load";
        WaitUntil([&] { return loaded(wave); }, failure);
    }
}

void JAudioDriver::GameFrame()
{
    // Once the frame process runs, the main loop counts the game's frames. Until then, GameFrame does.
    const uint64_t next = game_frames_ + 1;
    while (game_frames_ < next)
    {
        Frame();
        if (!ready_ && TimeReaches(next, kGameFramesPerSecond))
        {
            game_frames_ = next;
        }
    }
}

void JAudioDriver::RunMainLoop()
{
    while (ready_ && TimeReaches(game_frames_, kGameFramesPerSecond))
    {
        FrameProcess();
        ++game_frames_;
    }
}

} // namespace csf::game
