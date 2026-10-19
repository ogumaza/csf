// SPDX-License-Identifier: MIT

// Cafe OS's coreinit at a high level: the parts that the games' sound code and game mode's drivers use. The model
// covers threads, mutexes, semaphores, events, message queues and alarms on the machine's threads; time; frame heaps,
// expanded heaps and the default heap; the title's files, from the host; and the C library's memory functions. Time
// passes only when the host lets it. Each object that the guest passes in (an OSMutex, an OSEvent, an OSMessageQueue)
// is identified by its guest address. The object's state is kept here.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "cafe/machine.h"

namespace csf::cafe
{

class Coreinit
{
public:
    // Registers the handlers with `machine`. The machine must not have loaded its executable yet.
    explicit Coreinit(Machine& machine);

    Coreinit(const Coreinit&) = delete;
    Coreinit& operator=(const Coreinit&) = delete;

    // After the executable is loaded: the default heap, `heap_size` bytes of the machine's heap behind the functions
    // whose addresses the executable imports, and the main thread's OSThread.
    void Start(uint32_t heap_size);

    // The time in ticks since the system started, and its passing. Alarms that come due meanwhile run in order. They
    // run as calls from the host between threads.
    uint64_t Time() const
    {
        return time_;
    }

    void AdvanceTime(uint64_t ticks);

    // A file the guest can open, by its path (such as /vol/content/sound/x.bfsar). As on the console, the path's case
    // doesn't matter: a game may name a folder in another case than the title does.
    void AddFile(const std::string& path, std::vector<uint8_t> bytes);

    // The default heap, for the host's objects in the guest. Each block that Alloc gives holds zeros. Alloc throws
    // std::runtime_error when the heap is full.
    uint32_t Alloc(uint32_t size, uint32_t align = 16);
    void Free(uint32_t address);

private:
    struct Mutex
    {
        // OSTryLockMutex: locks the mutex for `thread` if the mutex is free or `thread` holds it already. Returns
        // false, with no change, if another thread holds the mutex.
        bool TryLock(Machine::Thread* thread);

        Machine::Thread* owner = nullptr;
        uint32_t count = 0;
    };

    struct Event
    {
        // Whether a thread that began to wait on the event after `seen` signals may go on: the event is signalled, or
        // it's a manual-reset event that has been signalled since then.
        bool Wakes(uint64_t seen) const;

        // A thread goes on past the event: an auto-reset event resets.
        void Take();

        bool signalled = false;
        bool auto_reset = false;
        uint64_t signals = 0; // OSSignalEvent's calls so far
    };

    struct Queue
    {
        std::deque<std::array<uint8_t, 16>> messages;
        uint32_t capacity = 0;
    };

    struct Alarm
    {
        bool set = false;
        uint64_t due = 0;
        uint64_t period = 0;
        uint32_t callback = 0;
        uint32_t user_data = 0;
        uint64_t order = 0; // when it was set: the order of alarms due at once
    };

    // A frame heap: allocations from its head up and from its tail down. The allocations at each end are freed
    // together.
    struct FrameHeap
    {
        // An allocation aligned to `align`, from the head or, with `from_tail`, the tail; 0 if nothing fits.
        uint32_t Allocate(uint32_t size, uint32_t align, bool from_tail);

        // Frees the allocations from the head if `mode` has bit 0 set, and those from the tail if it has bit 1 set, as
        // MEMFreeToFrmHeap does.
        void Free(uint32_t mode);

        // The largest allocation from the head, aligned to `align`, that fits.
        uint32_t AllocatableSize(uint32_t align) const;

        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t head = 0;
        uint32_t tail = 0;
    };

    // An expanded heap: its free ranges and its allocations, by address, each with its size.
    struct ExpHeap
    {
        // An allocation aligned to `align`, from the lowest free address or, with `from_tail`, the highest; 0 if
        // nothing fits.
        uint32_t Allocate(uint32_t size, uint32_t align, bool from_tail);

        bool Release(uint32_t address);

        // The free bytes in all, and the largest allocation aligned to `align` that fits.
        uint32_t TotalFreeSize() const;
        uint32_t AllocatableSize(uint32_t align) const;

        uint32_t start = 0;
        uint32_t end = 0;
        std::map<uint32_t, uint32_t> free;
        std::map<uint32_t, uint32_t> used;
    };

    struct OpenFile
    {
        const std::vector<uint8_t>* bytes = nullptr;
        uint32_t position = 0;
    };

    void RegisterThreads();
    void RegisterReports();
    void RegisterLocks();
    void RegisterSemaphores();
    void RegisterEvents();
    void RegisterMessageQueues();
    void RegisterAlarms();
    void RegisterTime();
    void RegisterMemory();
    void RegisterHeaps();
    void RegisterRuntime();
    void RegisterFiles();

    // A 64-bit argument. A call passes it in an aligned pair of registers from r`first`.
    uint64_t Arg64(int first) const;

    void Return64(uint64_t value);

    Machine::Thread& ThreadAt(uint32_t object);

    // The file that the guest names by `path`, whatever the case of the path's ASCII letters (see AddFile), or nullptr.
    const std::vector<uint8_t>* FindFile(const std::string& path) const;

    Machine& machine_;
    uint64_t time_ = 0;
    uint64_t alarm_order_ = 0;
    uint32_t system_info_ = 0;
    std::map<uint32_t, Mutex> mutexes_;
    std::map<uint32_t, Event> events_;
    std::map<uint32_t, Queue> queues_;
    std::map<uint32_t, uint64_t> sleep_queues_; // OSThreadQueue: each wake-up advances its count
    std::map<uint32_t, Alarm> alarms_;
    std::map<uint32_t, FrameHeap> frame_heaps_;
    std::map<std::string, std::vector<uint8_t>> files_;
    std::map<uint32_t, OpenFile> open_files_;
    uint32_t next_file_ = 1;

    // The default heap's handle, and every expanded heap (the default heap among them) by its handle.
    uint32_t default_heap_ = 0;
    std::map<uint32_t, ExpHeap> exp_heaps_;

    std::map<uint32_t, uint32_t> semaphores_;
    uint32_t errno_ = 0;
    uint32_t file_lock_ = 0; // the one lock word every file shares
};

} // namespace csf::cafe
