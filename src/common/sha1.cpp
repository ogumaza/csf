// SPDX-License-Identifier: MIT

#include "common/sha1.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "common/bytes.h"

namespace csf
{
namespace
{

uint32_t Rotate(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

} // namespace

Sha1::Sha1() : state_{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0}
{
}

void Sha1::Update(std::span<const uint8_t> data)
{
    length_ += data.size();
    while (!data.empty())
    {
        const size_t n = std::min(data.size(), buffer_.size() - buffered_);
        std::copy_n(data.begin(), n, buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        buffered_ += n;
        data = data.subspan(n);

        if (buffered_ == buffer_.size())
        {
            Block(buffer_.data());
            buffered_ = 0;
        }
    }
}

Sha1::Digest Sha1::Finish()
{
    // Pad with a 1 bit, zeros up to 8 bytes short of a block, and the length in bits.
    const uint64_t bits = length_ * 8;
    const uint8_t kOne = 0x80;
    Update({&kOne, 1});

    const uint8_t kZeros[64] = {};
    Update({kZeros, (buffer_.size() * 2 - 8 - buffered_) % buffer_.size()});

    uint8_t length[8];
    StoreBe64(length, bits);
    Update(length);

    Digest digest;
    for (size_t i = 0; i < state_.size(); ++i)
    {
        StoreBe32(digest.data() + i * 4, state_[i]);
    }

    return digest;
}

Sha1::Digest Sha1::Of(std::span<const uint8_t> data)
{
    Sha1 sha1;
    sha1.Update(data);

    return sha1.Finish();
}

std::string Sha1::Hex(const Digest& digest)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t byte : digest)
    {
        out += kDigits[byte >> 4];
        out += kDigits[byte & 15];
    }

    return out;
}

void Sha1::Block(const uint8_t* block)
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
    {
        w[i] = LoadBe32(block + i * 4);
    }
    for (int i = 16; i < 80; ++i)
    {
        w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    for (int i = 0; i < 80; ++i)
    {
        uint32_t f;
        uint32_t k;
        if (i < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        const uint32_t t = Rotate(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotate(b, 30);
        b = a;
        a = t;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

} // namespace csf
