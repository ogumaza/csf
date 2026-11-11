// SPDX-License-Identifier: MIT

// Children of a JAudio2 sound's track 0 at one volume: what a rip's volumes file holds (csf::ChildVolumes), and what
// the JAudio2 driver sets after it starts the sound (game::GameDriver::SetChildVolumes). A status by section: what a
// rip's sections file holds (csf::SectionStatuses), and what the driver writes in each frame
// (game::GameDriver::SetSectionStatuses).

#pragma once

#include <cstdint>

namespace csf
{

struct ChildVolume
{
    bool operator==(const ChildVolume&) const = default;

    uint32_t children = 0; // bit n for child n
    float volume = 1.0f;
};

struct SectionStatus
{
    bool operator==(const SectionStatus&) const = default;

    uint8_t section = 0; // the value of port 10 of the sound's root track
    uint8_t status = 0;  // the value that the driver writes to port 9
};

} // namespace csf
