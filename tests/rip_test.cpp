// SPDX-License-Identifier: MIT

// Tests the ripper's length analysis on the length cases (ntdlib/testing/length_cases.h): sequences that each take one
// of the analysis's paths, in an archive in the Wii U's byte order. The test also checks that the ripper recognises
// other consoles' sound archives by their first bytes, and reads the names of a JAudio1 game's sounds.

#include "rip/rip.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "ntdlib/testing/jaudio_files.h"
#include "ntdlib/testing/length_cases.h"
#include "ntdlib/testing/sar_files.h"
#include "xsflib/testing/check.h"

#include "csf/sar.h"
#include "rip/jaudio.h"

namespace length_cases = ntdlib::testing::length_cases;
namespace jaudio_files = ntdlib::testing::jaudio_files;
namespace sar_files = ntdlib::testing::sar_files;

using xsflib::testing::Check;

namespace
{

// Checks which files ForeignArchive takes for other consoles' sound archives. The Wii U's archive and the Switch's are
// built in their byte orders.
void CheckForeignArchives()
{
    const auto head = [](const std::vector<uint8_t>& file)
    {
        return std::string(file.begin(), file.begin() + 8);
    };

    const std::string wii_u = head(sar_files::Archive(true, {}));
    const std::string switch_archive = head(sar_files::Archive(false, {}));

    Check(!csf::rip::ForeignArchive(wii_u).has_value(), "a Wii U archive isn't another console's");
    Check(csf::rip::ForeignArchive(switch_archive) == "a Switch sound archive (little-endian FSAR)",
          "a Switch archive is the Switch's");
    Check(csf::rip::ForeignArchive("CSAR\xFF\xFE") == "a 3DS sound archive (CSAR)", "a 3DS archive is the 3DS's");
    Check(csf::rip::ForeignArchive("RSAR\xFE\xFF") == "a Wii sound archive (RSAR)", "a Wii archive is the Wii's");
    Check(!csf::rip::ForeignArchive("FSA").has_value(), "three bytes aren't an archive");
}

// A JaiInfo.asn with 18 categories and `sounds`. A name of 28 bytes fills its field with no zero after it.
std::vector<uint8_t> Asn(const std::vector<csf::rip::AsnSound>& sounds)
{
    std::vector<uint8_t> asn(16 + 32 * (18 + sounds.size()));
    asn[15] = static_cast<uint8_t>(sounds.size());
    for (std::size_t i = 0; i < 18; ++i)
    {
        asn[16 + 32 * i] = 'C';
    }

    for (std::size_t i = 0; i < sounds.size(); ++i)
    {
        uint8_t* record = asn.data() + 16 + 32 * (18 + i);
        std::copy(sounds[i].name.begin(), sounds[i].name.end(), record);
        for (int byte = 0; byte < 4; ++byte)
        {
            record[28 + byte] = static_cast<uint8_t>(sounds[i].id >> (24 - 8 * byte));
        }
    }

    return asn;
}

// Checks the reader of JaiInfo.asn, and that a JAudio1 game's rip offers the sequences of its sound table but the
// effects sequence, with the names that JaiInfo.asn gives them.
void CheckAsnSounds()
{
    const std::string long_name(28, 'N');
    const std::vector<uint8_t> asn = Asn({{0x80000800, "JA_BGM_SE_SEQUENCE"},
                                          {0x80000001, "JA_BGM_FIRST"},
                                          {0x80000002, long_name},
                                          {0x00001234, "JA_SE_EFFECT"},
                                          {0xc0000000, "JA_STRM_STREAM"}});

    const std::vector<csf::rip::AsnSound> sounds = csf::rip::ReadAsnSounds(asn);

    Check(sounds.size() == 5, "the file names 5 sounds");
    Check(sounds.size() == 5 && sounds[1].id == 0x80000001 && sounds[1].name == "JA_BGM_FIRST",
          "a sound's record gives its ID and name");
    Check(sounds.size() == 5 && sounds[2].name == long_name, "a name can fill its 28 bytes");

    bool threw = false;
    try
    {
        csf::rip::ReadAsnSounds(std::span(asn).first(asn.size() - 1));
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }

    Check(threw, "a file too short for its sounds is an error");

    // The sound table: the effects sequence in file 7, two pieces of music and an effect.
    csf::rip::JAudioGame game{};
    game.name = "a JAudio1 game";
    game.driver = csf::GameDriverKind::kJAudio1;
    game.startup_archive = "content/Sound/JaiInit.aaf";
    game.name_file = "content/Sound/JaiInfo.asn";
    const std::map<std::string, std::vector<uint8_t>> content = {
        {"Sound/JaiInit.aaf", jaudio_files::AafFile({{16, 127, 7}, {16, 100, 3}, {16, 90, 9}, {0, 80}}, {})},
        {"Sound/JaiInfo.asn", asn}};

    const std::vector<ntdlib::jaudio::SoundEntry> offered = csf::rip::JAudioSounds(game, content);

    Check(offered.size() == 2 && offered[0].Id() == 0x80000001 && offered[0].name == "JA_BGM_FIRST" &&
              offered[1].Id() == 0x80000002 && offered[1].name == long_name,
          "a JAudio1 rip offers the sequences but the effects sequence");
}

} // namespace

int main()
{
    // CSF's analysis waits for the loops of the tracks other than the main track (its model's OthersLooped).
    const csf::rip::LengthAnalyzer analyzer(
        csf::fsar::SoundArchive::Load(length_cases::Archive(true), csf::kSarOptions));
    length_cases::CheckCases([&analyzer](uint32_t index) { return analyzer.Analyze(index); }, true);

    CheckForeignArchives();
    CheckAsnSounds();

    return xsflib::testing::Summary("rip");
}
