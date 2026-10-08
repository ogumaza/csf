// SPDX-License-Identifier: MIT

// SHA-1 (FIPS 180-4).

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace csf
{

class Sha1
{
public:
    using Digest = std::array<uint8_t, 20>;

    Sha1();

    // Adds data to the message.
    void Update(std::span<const uint8_t> data);

    // Finishes the message and returns its hash. The object can't be updated afterwards.
    Digest Finish();

    // The hash of `data` alone.
    static Digest Of(std::span<const uint8_t> data);

    // A hash as 40 lowercase hex digits.
    static std::string Hex(const Digest& digest);

private:
    void Block(const uint8_t* block);

    std::array<uint32_t, 5> state_;
    std::array<uint8_t, 64> buffer_{};
    size_t buffered_ = 0;
    uint64_t length_ = 0; // bytes added so far
};

} // namespace csf
