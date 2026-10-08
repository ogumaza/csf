// SPDX-License-Identifier: MIT

// ASCII lowercasing for tag names, file names and extensions. Unlike std::tolower, it doesn't depend on the C locale. A
// host program such as foobar2000 may have changed the locale.

#pragma once

#include <string>

namespace csf
{

inline std::string AsciiLower(std::string s)
{
    for (char& c : s)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }

    return s;
}

} // namespace csf
