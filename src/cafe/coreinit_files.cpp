// SPDX-License-Identifier: MIT

// coreinit's file system: the title's files, as the guest opens, reads and looks them up.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"

#include "cafe/coreinit.h"
#include "cafe/machine.h"
#include "cafe/memory.h"

namespace csf::cafe
{
namespace
{

// FS's results.
constexpr uint32_t kFsOk = 0;
constexpr uint32_t kFsNotFound = static_cast<uint32_t>(-6);

constexpr uint32_t kStatSize = 0x64;

// Copies up to `count` whole elements of `size` bytes from `bytes`, from `position` on, to `buffer` in the guest's
// memory. Returns the number of elements copied: no more than the file holds from `position`.
uint32_t ReadElements(Memory& memory, uint32_t buffer, const std::vector<uint8_t>& bytes, uint32_t position,
                      uint32_t size, uint32_t count)
{
    const auto file_size = static_cast<uint32_t>(bytes.size());
    const uint32_t from = std::min(position, file_size);
    const uint32_t elements = std::min(count, (file_size - from) / size);
    if (elements != 0)
    {
        std::memcpy(memory.Pointer(buffer, elements * size), bytes.data() + from, elements * size);
    }

    return elements;
}

// Writes the FSStat of a file of `size` bytes at `stat`: zeros, with the size at +0x10.
void WriteStat(Memory& memory, uint32_t stat, std::size_t size)
{
    std::memset(memory.Pointer(stat, kStatSize), 0, kStatSize);
    memory.Write32(stat + 0x10, static_cast<uint32_t>(size));
}

} // namespace

void Coreinit::AddFile(const std::string& path, std::vector<uint8_t> bytes)
{
    // The console's file system ignores the case of ASCII letters. The table keeps the paths in lowercase.
    files_[xsflib::AsciiLower(path)] = std::move(bytes);
}

const Coreinit::Files::value_type* Coreinit::FindFile(const std::string& path) const
{
    const auto file = files_.find(xsflib::AsciiLower(path));
    return file != files_.end() ? &*file : nullptr;
}

void Coreinit::RegisterFileSystem()
{
    Machine& machine = machine_;

    // The file system's start and its clients. Every client reads the same files.
    machine.Register("coreinit", "FSInit", [](Machine&) {});
    machine.Register("coreinit", "FSAddClient", [](Machine& m) { m.Return(kFsOk); });
    machine.Register("coreinit", "FSDelClient", [](Machine& m) { m.Return(kFsOk); });
    machine.Register("coreinit", "FSSetStateChangeNotification", [](Machine&) {});
    machine.Register("coreinit", "FSInitCmdBlock", [](Machine&) {});
    machine.Register("coreinit", "FSSetCmdPriority", [](Machine& m) { m.Return(kFsOk); });

    const auto fs_get_cwd = [](Machine& m)
    {
        const std::string kCwd = kGuestContentFolder;
        if (m.Arg(3) > kCwd.size())
        {
            m.memory.WriteString(m.Arg(2), kCwd);
        }
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSGetCwd", fs_get_cwd);
}

void Coreinit::RegisterFileReads()
{
    Machine& machine = machine_;

    // FSOpenFile(client, block, path, mode, handle out, errors).
    const auto fs_open_file = [this](Machine& m)
    {
        const Files::value_type* const file = FindFile(m.memory.ReadString(m.Arg(2)));
        if (file == nullptr)
        {
            m.Return(kFsNotFound);
            return;
        }

        const uint32_t handle = next_file_++;
        open_files_[handle] = {file, 0};
        m.memory.Write32(m.Arg(4), handle);
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSOpenFile", fs_open_file);

    const auto fs_close_file = [this](Machine& m)
    {
        open_files_.erase(m.Arg(2));
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSCloseFile", fs_close_file);

    // FSReadFile(client, block, buffer, size, count, handle, flags, errors): the number of whole elements read.
    const auto fs_read_file = [this](Machine& m)
    {
        const auto file = open_files_.find(m.Arg(5));
        const uint32_t size = m.Arg(3);
        if (file == open_files_.end() || size == 0)
        {
            m.Return(kFsNotFound);
            return;
        }

        OpenFile& f = file->second;
        files_read_.insert(f.file->first);
        const uint32_t count = ReadElements(m.memory, m.Arg(2), f.file->second, f.position, size, m.Arg(4));
        f.position += count * size;
        m.Return(count);
    };
    machine.Register("coreinit", "FSReadFile", fs_read_file);

    // FSReadFileWithPos(client, block, buffer, size, count, position, handle, flags, errors): FSReadFile's read from
    // `position`. The file's position stays where it was.
    const auto fs_read_file_with_pos = [this](Machine& m)
    {
        const auto file = open_files_.find(m.Arg(6));
        const uint32_t size = m.Arg(3);
        if (file == open_files_.end() || size == 0)
        {
            m.Return(kFsNotFound);
            return;
        }

        const OpenFile& f = file->second;
        files_read_.insert(f.file->first);
        m.Return(ReadElements(m.memory, m.Arg(2), f.file->second, m.Arg(5), size, m.Arg(4)));
    };
    machine.Register("coreinit", "FSReadFileWithPos", fs_read_file_with_pos);

    const auto fs_set_pos_file = [this](Machine& m)
    {
        const auto file = open_files_.find(m.Arg(2));
        if (file == open_files_.end())
        {
            m.Return(kFsNotFound);
            return;
        }

        file->second.position = m.Arg(3);
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSSetPosFile", fs_set_pos_file);
}

void Coreinit::RegisterFileStats()
{
    Machine& machine = machine_;

    // FSGetStat(client, block, path, stat out, errors): FSGetStatFile's answer for a file by its path.
    const auto fs_get_stat = [this](Machine& m)
    {
        const Files::value_type* const file = FindFile(m.memory.ReadString(m.Arg(2)));
        if (file == nullptr)
        {
            m.Return(kFsNotFound);
            return;
        }

        WriteStat(m.memory, m.Arg(3), file->second.size());
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSGetStat", fs_get_stat);

    // FSGetStatFile(client, block, handle, stat out, errors).
    const auto fs_get_stat_file = [this](Machine& m)
    {
        const auto file = open_files_.find(m.Arg(2));
        if (file == open_files_.end())
        {
            m.Return(kFsNotFound);
            return;
        }

        WriteStat(m.memory, m.Arg(3), file->second.file->second.size());
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSGetStatFile", fs_get_stat_file);
}

} // namespace csf::cafe
