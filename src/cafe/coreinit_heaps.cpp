// SPDX-License-Identifier: MIT

// coreinit's heaps: frame heaps, expanded heaps and the default heap, and the guest's functions that use them.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include "cafe/coreinit.h"
#include "cafe/machine.h"
#include "cafe/memory.h"

namespace csf::cafe
{
namespace
{

// MEMFreeToFrmHeap's modes.
constexpr uint32_t kFreeHead = 1;
constexpr uint32_t kFreeTail = 2;

// The bytes at the start of a frame heap or an expanded heap that the heap's header takes. No allocation uses them.
constexpr uint32_t kHeapHeader = 0x40;

// The alignment that a heap function's signed alignment asks for: its magnitude. A negative alignment takes the block
// from the heap's tail.
uint32_t Alignment(int32_t align)
{
    return static_cast<uint32_t>(align < 0 ? -align : align);
}

} // namespace

void Coreinit::StartDefaultHeap(uint32_t heap_size)
{
    const uint32_t base = machine_.Allocate(heap_size, 64);
    default_heap_ = base;
    ExpHeap& heap = exp_heaps_[base];
    heap.start = base;
    heap.end = base + heap_size;
    heap.free[base] = heap_size;

    const auto set = [&](const char* name, Machine::Handler handler)
    {
        if (const uint32_t variable = machine_.DataImport("coreinit", name))
        {
            machine_.memory.Write32(variable, machine_.MakeTrap(std::string("coreinit.") + name, std::move(handler)));
        }
    };

    set("MEMAllocFromDefaultHeap", [this](Machine& m) { m.Return(Alloc(m.Arg(0), 8)); });

    const auto mem_alloc_from_default_heap_ex = [this](Machine& m)
    {
        const auto align = static_cast<int32_t>(m.Arg(1));
        m.Return(Alloc(m.Arg(0), std::max(Alignment(align), 4u)));
    };
    set("MEMAllocFromDefaultHeapEx", mem_alloc_from_default_heap_ex);

    set("MEMFreeToDefaultHeap", [this](Machine& m) { Free(m.Arg(0)); });
}

uint32_t Coreinit::FrameHeap::Allocate(uint32_t size, uint32_t align, bool from_tail)
{
    if (!from_tail)
    {
        const uint32_t at = AlignUp(head, std::max(align, 4u));
        if (at > tail || size > tail - at)
        {
            return 0;
        }

        head = at + size;
        return at;
    }

    if (size > tail - head)
    {
        return 0;
    }

    const uint32_t at = AlignDown(tail - size, align);
    if (at < head)
    {
        return 0;
    }

    tail = at;

    return at;
}

void Coreinit::FrameHeap::Free(uint32_t mode)
{
    if (mode & kFreeHead)
    {
        head = start + kHeapHeader;
    }
    if (mode & kFreeTail)
    {
        tail = end;
    }
}

uint32_t Coreinit::FrameHeap::AllocatableSize(uint32_t align) const
{
    const uint32_t at = AlignUp(head, std::max(align, 4u));

    return at < tail ? tail - at : 0;
}

uint32_t Coreinit::ExpHeap::Allocate(uint32_t size, uint32_t align, bool from_tail)
{
    size = AlignUp(std::max<uint32_t>(size, 1), 4);
    align = std::max<uint32_t>(align, 4);

    const auto take = [&](std::map<uint32_t, uint32_t>::iterator range, uint32_t at)
    {
        const uint32_t begin = range->first;
        const uint32_t finish = range->first + range->second;
        free.erase(range);
        if (at != begin)
        {
            free[begin] = at - begin;
        }
        if (at + size != finish)
        {
            free[at + size] = finish - (at + size);
        }
        used[at] = size;

        return at;
    };

    if (!from_tail)
    {
        for (auto range = free.begin(); range != free.end(); ++range)
        {
            const uint32_t at = AlignUp(range->first, align);
            if (at >= range->first && at - range->first <= range->second && size <= range->second - (at - range->first))
            {
                return take(range, at);
            }
        }
    }
    else
    {
        for (auto range = free.rbegin(); range != free.rend(); ++range)
        {
            if (size > range->second)
            {
                continue;
            }

            const uint32_t at = AlignDown(range->first + range->second - size, align);
            if (at >= range->first)
            {
                return take(std::prev(range.base()), at);
            }
        }
    }

    return 0;
}

bool Coreinit::ExpHeap::Release(uint32_t address)
{
    const auto block = used.find(address);
    if (block == used.end())
    {
        return false;
    }

    uint32_t begin = address;
    uint32_t size = block->second;
    used.erase(block);

    // Merge with the free ranges on either side.
    const auto after = free.find(begin + size);
    if (after != free.end())
    {
        size += after->second;
        free.erase(after);
    }
    auto before = free.lower_bound(begin);
    if (before != free.begin() && std::prev(before)->first + std::prev(before)->second == begin)
    {
        --before;
        begin = before->first;
        size += before->second;
        free.erase(before);
    }
    free[begin] = size;

    return true;
}

uint32_t Coreinit::ExpHeap::TotalFreeSize() const
{
    uint32_t total = 0;
    for (const auto& [address, size] : free)
    {
        total += size;
    }

    return total;
}

uint32_t Coreinit::ExpHeap::AllocatableSize(uint32_t align) const
{
    align = std::max<uint32_t>(align, 4);
    uint32_t largest = 0;
    for (const auto& [address, size] : free)
    {
        const uint32_t lost = AlignUp(address, align) - address;
        largest = std::max(largest, size > lost ? AlignDown(size - lost, 4) : 0u);
    }

    return largest;
}

uint32_t Coreinit::Alloc(uint32_t size, uint32_t align)
{
    const uint32_t address = exp_heaps_.at(default_heap_).Allocate(size, align, false);
    if (address == 0)
    {
        throw std::runtime_error("game mode's default heap is full");
    }

    std::memset(machine_.memory.Pointer(address, size), 0, size);

    return address;
}

void Coreinit::Free(uint32_t address)
{
    if (address != 0)
    {
        exp_heaps_.at(default_heap_).Release(address);
    }
}

void Coreinit::RegisterFrameHeaps()
{
    Machine& machine = machine_;

    // Frame heaps: allocations from the head (a positive alignment) or the tail (a negative one). The allocations at
    // each end are freed together.
    const auto mem_create_frm_heap_ex = [this](Machine& m)
    {
        const uint32_t start = AlignUp(m.Arg(0), 4);
        const uint32_t end = AlignDown(m.Arg(0) + m.Arg(1), 4);
        if (end < start || end - start < kHeapHeader)
        {
            m.Return(0);
            return;
        }

        frame_heaps_[start] = {start, end, start + kHeapHeader, end};
        m.Return(start);
    };
    machine.Register("coreinit", "MEMCreateFrmHeapEx", mem_create_frm_heap_ex);

    const auto mem_destroy_frm_heap = [this](Machine& m)
    {
        frame_heaps_.erase(m.Arg(0));
        m.Return(m.Arg(0));
    };
    machine.Register("coreinit", "MEMDestroyFrmHeap", mem_destroy_frm_heap);

    const auto mem_alloc_from_frm_heap_ex = [this](Machine& m)
    {
        const auto heap = frame_heaps_.find(m.Arg(0));
        const auto align = static_cast<int32_t>(m.Arg(2));
        m.Return(heap == frame_heaps_.end() ? 0 : heap->second.Allocate(m.Arg(1), Alignment(align), align < 0));
    };
    machine.Register("coreinit", "MEMAllocFromFrmHeapEx", mem_alloc_from_frm_heap_ex);

    const auto mem_free_to_frm_heap = [this](Machine& m)
    {
        const auto heap = frame_heaps_.find(m.Arg(0));
        if (heap != frame_heaps_.end())
        {
            heap->second.Free(m.Arg(1));
        }
    };
    machine.Register("coreinit", "MEMFreeToFrmHeap", mem_free_to_frm_heap);

    const auto mem_get_allocatable_size_for_frm_heap_ex = [this](Machine& m)
    {
        const auto heap = frame_heaps_.find(m.Arg(0));
        const auto align = static_cast<int32_t>(m.Arg(1));
        m.Return(heap == frame_heaps_.end() ? 0 : heap->second.AllocatableSize(Alignment(align)));
    };
    machine.Register("coreinit", "MEMGetAllocatableSizeForFrmHeapEx", mem_get_allocatable_size_for_frm_heap_ex);
}

void Coreinit::RegisterExpHeaps()
{
    Machine& machine = machine_;

    // Expanded heaps: MEMCreateExpHeapEx(address, size, flags); allocations from the head (a positive alignment) or the
    // tail (a negative one).
    const auto mem_create_exp_heap_ex = [this](Machine& m)
    {
        const uint32_t start = AlignUp(m.Arg(0), 4);
        const uint32_t end = AlignDown(m.Arg(0) + m.Arg(1), 4);
        if (end <= start + kHeapHeader)
        {
            m.Return(0);
            return;
        }

        ExpHeap& heap = exp_heaps_[start];
        heap = ExpHeap();
        heap.start = start;
        heap.end = end;
        heap.free[start + kHeapHeader] = end - (start + kHeapHeader);
        m.Return(start);
    };
    machine.Register("coreinit", "MEMCreateExpHeapEx", mem_create_exp_heap_ex);

    const auto mem_destroy_exp_heap = [this](Machine& m)
    {
        exp_heaps_.erase(m.Arg(0));
        m.Return(m.Arg(0));
    };
    machine.Register("coreinit", "MEMDestroyExpHeap", mem_destroy_exp_heap);

    const auto mem_alloc_from_exp_heap_ex = [this](Machine& m)
    {
        const auto heap = exp_heaps_.find(m.Arg(0));
        const auto align = static_cast<int32_t>(m.Arg(2));
        m.Return(heap == exp_heaps_.end() ? 0 : heap->second.Allocate(m.Arg(1), Alignment(align), align < 0));
    };
    machine.Register("coreinit", "MEMAllocFromExpHeapEx", mem_alloc_from_exp_heap_ex);

    const auto mem_free_to_exp_heap = [this](Machine& m)
    {
        const auto heap = exp_heaps_.find(m.Arg(0));
        if (heap != exp_heaps_.end())
        {
            heap->second.Release(m.Arg(1));
        }
    };
    machine.Register("coreinit", "MEMFreeToExpHeap", mem_free_to_exp_heap);

    const auto mem_get_size_for_m_block_exp_heap = [this](Machine& m)
    {
        for (const auto& [handle, heap] : exp_heaps_)
        {
            if (const auto block = heap.used.find(m.Arg(0)); block != heap.used.end())
            {
                m.Return(block->second);
                return;
            }
        }

        m.Return(0);
    };
    machine.Register("coreinit", "MEMGetSizeForMBlockExpHeap", mem_get_size_for_m_block_exp_heap);

    const auto mem_get_total_free_size_for_exp_heap = [this](Machine& m)
    {
        const auto heap = exp_heaps_.find(m.Arg(0));
        m.Return(heap == exp_heaps_.end() ? 0 : heap->second.TotalFreeSize());
    };
    machine.Register("coreinit", "MEMGetTotalFreeSizeForExpHeap", mem_get_total_free_size_for_exp_heap);

    const auto mem_get_allocatable_size_for_exp_heap_ex = [this](Machine& m)
    {
        const auto heap = exp_heaps_.find(m.Arg(0));
        const auto align = static_cast<int32_t>(m.Arg(1));
        m.Return(heap == exp_heaps_.end() ? 0 : heap->second.AllocatableSize(Alignment(align)));
    };
    machine.Register("coreinit", "MEMGetAllocatableSizeForExpHeapEx", mem_get_allocatable_size_for_exp_heap_ex);
}

void Coreinit::RegisterHeapLookups()
{
    Machine& machine = machine_;

    // MEMFindContainHeap(address): the innermost heap that contains the address, or 0. A heap that a game makes in a
    // block of another heap starts above the other heap's start.
    const auto mem_find_contain_heap = [this](Machine& m)
    {
        const uint32_t address = m.Arg(0);
        uint32_t found = 0;
        uint32_t found_start = 0;
        const auto consider = [&](uint32_t handle, uint32_t start, uint32_t end)
        {
            if (address >= start && address < end && (found == 0 || start > found_start))
            {
                found = handle;
                found_start = start;
            }
        };

        for (const auto& [handle, heap] : exp_heaps_)
        {
            consider(handle, heap.start, heap.end);
        }

        for (const auto& [handle, heap] : frame_heaps_)
        {
            consider(handle, heap.start, heap.end);
        }

        m.Return(found);
    };
    machine.Register("coreinit", "MEMFindContainHeap", mem_find_contain_heap);

    // MEMGetBaseHeapHandle(arena): the default heap is MEM2's base heap (arena 1). Game mode has no base heap in the
    // other arenas, MEM1's and the foreground's. A JSystem game on sead's framework (The Wind Waker HD) asks for it as
    // it starts up.
    machine.Register("coreinit", "MEMGetBaseHeapHandle",
                     [this](Machine& m) { m.Return(m.Arg(0) == 1 ? default_heap_ : 0); });
}

} // namespace csf::cafe
