// SPDX-License-Identifier: MIT

// Checks SHA-1 against the test vectors of FIPS 180 and RFC 3174, and that feeding a message in pieces gives the same
// hash as feeding it whole.

#include "common/sha1.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "check.h"

namespace
{

std::string HashOf(const std::string& text)
{
    return csf::Sha1::Hex(csf::Sha1::Of({reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
}

} // namespace

int main()
{
    CSF_CHECK(HashOf("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CSF_CHECK(HashOf("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CSF_CHECK(HashOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CSF_CHECK(HashOf(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    // Pieces of every size from 1 to 130 bytes, across block boundaries.
    std::vector<uint8_t> message(1000);
    for (size_t i = 0; i < message.size(); ++i)
    {
        message[i] = static_cast<uint8_t>(i * 7 + 3);
    }

    const csf::Sha1::Digest whole = csf::Sha1::Of(message);
    for (size_t piece = 1; piece <= 130; ++piece)
    {
        csf::Sha1 sha1;
        for (size_t i = 0; i < message.size(); i += piece)
        {
            sha1.Update(std::span(message).subspan(i, std::min(piece, message.size() - i)));
        }

        Check(sha1.Finish() == whole, "pieces of " + std::to_string(piece) + " bytes");
    }

    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);

        return 1;
    }

    return 0;
}
