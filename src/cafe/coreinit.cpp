// SPDX-License-Identifier: MIT

#include "cafe/coreinit.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace csf::cafe
{
namespace
{

// The clocks OSGetSystemInfo reports.
constexpr uint32_t kBusClock = 248'625'000;
constexpr uint32_t kCoreClock = 1'243'125'000;

// Flags and modes.
constexpr uint32_t kMessageBlock = 1;
constexpr uint32_t kMessageHighPriority = 2;
constexpr uint32_t kEventAutoReset = 1;
constexpr uint32_t kFreeHead = 1;
constexpr uint32_t kFreeTail = 2;

// FS's results.
constexpr uint32_t kFsOk = 0;
constexpr uint32_t kFsNotFound = static_cast<uint32_t>(-6);

constexpr uint32_t kFrameHeapHeader = 0x40;
constexpr uint32_t kStatSize = 0x64;

uint32_t AlignUp(uint32_t value, uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

uint32_t AlignDown(uint32_t value, uint32_t align)
{
    return value & ~(align - 1);
}

} // namespace

Coreinit::Coreinit(Machine& machine) : machine_(machine)
{
    RegisterThreads();
    RegisterReports();
    RegisterSync();
    RegisterTime();
    RegisterMemory();
    RegisterHeaps();
    RegisterRuntime();
    RegisterFiles();
}

void Coreinit::Start(uint32_t heap_size)
{
    // The default heap.
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
    set("MEMAllocFromDefaultHeapEx",
        [this](Machine& m)
        {
            const auto align = static_cast<int32_t>(m.Arg(1));
            m.Return(Alloc(m.Arg(0), static_cast<uint32_t>(std::max(align < 0 ? -align : align, 4))));
        });
    set("MEMFreeToDefaultHeap", [this](Machine& m) { Free(m.Arg(0)); });

    // The main thread's OSThread, and what OSGetSystemInfo reports.
    machine_.MainThread().object = Alloc(0x6A0, 8);
    system_info_ = Alloc(0x20, 8);
    machine_.memory.Write32(system_info_, kBusClock);
    machine_.memory.Write32(system_info_ + 4, kCoreClock);
}

void Coreinit::AdvanceTime(uint64_t ticks)
{
    const uint64_t target = time_ + ticks;
    for (;;)
    {
        // The next alarm due by the target: the earliest, and the earliest set among those due at once.
        auto next = alarms_.end();
        for (auto it = alarms_.begin(); it != alarms_.end(); ++it)
        {
            if (it->second.set && it->second.due <= target &&
                (next == alarms_.end() || it->second.due < next->second.due ||
                 (it->second.due == next->second.due && it->second.order < next->second.order)))
            {
                next = it;
            }
        }
        if (next == alarms_.end())
        {
            break;
        }
        const uint32_t address = next->first;
        Alarm& alarm = next->second;
        time_ = std::max(time_, alarm.due);
        if (alarm.period != 0)
        {
            alarm.due += alarm.period;
        }
        else
        {
            alarm.set = false;
        }
        machine_.Interrupt(alarm.callback, {address, 0});
    }
    time_ = target;
}

void Coreinit::AddFile(const std::string& path, std::vector<uint8_t> bytes)
{
    files_[path] = std::move(bytes);
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

uint64_t Coreinit::Arg64(int first) const
{
    return (uint64_t{machine_.Arg(first)} << 32) | machine_.Arg(first + 1);
}

void Coreinit::Return64(uint64_t value)
{
    machine_.cpu.state_.gpr[3] = static_cast<uint32_t>(value >> 32);
    machine_.cpu.state_.gpr[4] = static_cast<uint32_t>(value);
}

Machine::Thread& Coreinit::ThreadAt(uint32_t object)
{
    Machine::Thread* const thread = machine_.FindThread(object);
    if (thread == nullptr)
    {
        throw std::runtime_error("the guest named a thread that game mode doesn't have");
    }

    return *thread;
}

void Coreinit::RegisterThreads()
{
    Machine& m = machine_;

    // OSCreateThread(thread, entry, argc, argv, stack top, stack size, priority, attributes).
    m.Register("coreinit", "OSCreateThread",
               [](Machine& m)
               {
                   m.CreateThread("thread", m.Arg(0), m.Arg(1), m.Arg(2), m.Arg(3), m.Arg(4),
                                  static_cast<int>(m.Arg(6)));
                   m.Return(1);
               });
    m.Register("coreinit", "OSResumeThread",
               [this](Machine& m)
               {
                   Machine::Thread& thread = ThreadAt(m.Arg(0));
                   const bool was_suspended = !thread.started;
                   thread.started = true;
                   m.Return(was_suspended ? 1 : 0);
                   m.Yield();
               });
    m.Register("coreinit", "OSJoinThread",
               [this](Machine& m)
               {
                   Machine::Thread* const thread = &ThreadAt(m.Arg(0));
                   const uint32_t result = m.Arg(1);
                   const auto finish = [&m, thread, result]
                   {
                       if (result != 0)
                       {
                           m.memory.Write32(result, thread->exit_value);
                       }
                       m.Return(1);
                   };
                   if (thread->exited)
                   {
                       finish();
                       return;
                   }
                   m.Wait([thread] { return thread->exited; }, finish);
               });
    m.Register("coreinit", "OSExitThread", [](Machine& m) { m.ExitThread(m.Arg(0)); });
    m.Register("coreinit", "OSGetCurrentThread",
               [](Machine& m) { m.Return(m.Current() != nullptr ? m.Current()->object : m.MainThread().object); });
    m.Register("coreinit", "OSSetThreadName",
               [this](Machine& m)
               {
                   if (m.Arg(1) != 0)
                   {
                       ThreadAt(m.Arg(0)).name = m.memory.ReadString(m.Arg(1));
                   }
               });
    m.Register("coreinit", "OSSetThreadPriority",
               [this](Machine& m)
               {
                   ThreadAt(m.Arg(0)).priority = static_cast<int>(m.Arg(1));
                   m.Return(1);
                   m.Yield();
               });
    m.Register("coreinit", "OSIsThreadTerminated", [this](Machine& m) { m.Return(ThreadAt(m.Arg(0)).exited); });

    m.Register("coreinit", "OSGetThreadPriority",
               [this](Machine& m) { m.Return(static_cast<uint32_t>(ThreadAt(m.Arg(0)).priority)); });
    // OSSleepTicks(ticks): the thread waits while that much time passes.
    m.Register("coreinit", "OSSleepTicks",
               [this](Machine& m)
               {
                   const uint64_t deadline = time_ + Arg64(0);
                   m.Wait([this, deadline] { return time_ >= deadline; });
               });
    m.Register("coreinit", "OSYieldThread", [](Machine&) {});

    // The guest's threads all take turns on one core here: the main core. On a Wii U, that's core 1.
    m.Register("coreinit", "OSGetCoreId", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSGetMainCoreId", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSIsMainCore", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSGetCoreCount", [](Machine& m) { m.Return(3); });
    m.Register("coreinit", "OSSetThreadAffinity", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSGetThreadAffinity", [](Machine& m) { m.Return(7); });

    // A thread queue: OSSleepThread waits until the next OSWakeupThread.
    m.Register("coreinit", "OSInitThreadQueue", [this](Machine& m) { sleep_queues_[m.Arg(0)] = 0; });
    m.Register("coreinit", "OSSleepThread",
               [this](Machine& m)
               {
                   uint64_t* const wakes = &sleep_queues_[m.Arg(0)];
                   const uint64_t now = *wakes;
                   m.Wait([wakes, now] { return *wakes != now; });
               });
    m.Register("coreinit", "OSWakeupThread",
               [this](Machine& m)
               {
                   ++sleep_queues_[m.Arg(0)];
                   m.Yield();
               });
}

void Coreinit::RegisterReports()
{
    Machine& m = machine_;

    // The guest's messages: reports are dropped. A panic or fatal error stops the guest with its message.
    for (const char* name : {"OSReport", "OSReportInfo", "OSReportWarn", "OSReportVerbose", "COSInfo", "COSWarn",
                             "COSError", "COSVerbose"})
    {
        m.Register("coreinit", name, [](Machine&) {});
    }
    m.Register("coreinit", "OSPanic",
               [](Machine& m)
               {
                   throw std::runtime_error("the guest panicked: " + m.memory.ReadString(m.Arg(2)) + " (" +
                                            m.memory.ReadString(m.Arg(0)) + ")");
               });
    m.Register("coreinit", "OSFatal",
               [](Machine& m) { throw std::runtime_error("the guest stopped: " + m.memory.ReadString(m.Arg(0))); });
}

void Coreinit::RegisterSync()
{
    Machine& m = machine_;

    // The C library's global lock. The lock can't be contended while the threads take turns on one core.
    m.Register("coreinit", "__ghsLock", [](Machine&) {});
    m.Register("coreinit", "__ghsUnlock", [](Machine&) {});

    m.Register("coreinit", "OSInitMutex", [this](Machine& m) { mutexes_[m.Arg(0)] = Mutex(); });
    m.Register("coreinit", "OSLockMutex",
               [this](Machine& m)
               {
                   Mutex* const mutex = &mutexes_[m.Arg(0)];
                   if (mutex->owner == nullptr || mutex->owner == m.Current())
                   {
                       mutex->owner = m.Current();
                       ++mutex->count;
                       return;
                   }
                   m.Wait([mutex] { return mutex->owner == nullptr; },
                          [mutex, &m]
                          {
                              mutex->owner = m.Current();
                              mutex->count = 1;
                          });
               });
    m.Register("coreinit", "OSUnlockMutex",
               [this](Machine& m)
               {
                   Mutex& mutex = mutexes_[m.Arg(0)];
                   if (mutex.owner == m.Current() && mutex.count != 0 && --mutex.count == 0)
                   {
                       mutex.owner = nullptr;
                       m.Yield();
                   }
               });

    m.Register("coreinit", "OSTryLockMutex",
               [this](Machine& m)
               {
                   Mutex& mutex = mutexes_[m.Arg(0)];
                   if (mutex.owner == nullptr || mutex.owner == m.Current())
                   {
                       mutex.owner = m.Current();
                       ++mutex.count;
                       m.Return(1);
                       return;
                   }
                   m.Return(0);
               });
    // OSFastMutex: an OSMutex by another name, here.
    m.Register("coreinit", "OSFastMutex_Init", [this](Machine& m) { mutexes_[m.Arg(0)] = Mutex(); });
    m.Register("coreinit", "OSFastMutex_Lock",
               [this](Machine& m)
               {
                   Mutex* const mutex = &mutexes_[m.Arg(0)];
                   if (mutex->owner == nullptr || mutex->owner == m.Current())
                   {
                       mutex->owner = m.Current();
                       ++mutex->count;
                       return;
                   }
                   m.Wait([mutex] { return mutex->owner == nullptr; },
                          [mutex, &m]
                          {
                              mutex->owner = m.Current();
                              mutex->count = 1;
                          });
               });
    m.Register("coreinit", "OSFastMutex_Unlock",
               [this](Machine& m)
               {
                   Mutex& mutex = mutexes_[m.Arg(0)];
                   if (mutex.owner == m.Current() && mutex.count != 0 && --mutex.count == 0)
                   {
                       mutex.owner = nullptr;
                       m.Yield();
                   }
               });
    // Spin locks can't be contended while the threads take turns on one core.
    m.Register("coreinit", "OSInitSpinLock", [](Machine&) {});
    m.Register("coreinit", "OSAcquireSpinLock", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSReleaseSpinLock", [](Machine& m) { m.Return(1); });

    // Semaphores: OSInitSemaphore(semaphore, count); a wait takes one when there's one to take.
    m.Register("coreinit", "OSInitSemaphore", [this](Machine& m) { semaphores_[m.Arg(0)] = m.Arg(1); });
    m.Register("coreinit", "OSWaitSemaphore",
               [this](Machine& m)
               {
                   uint32_t* const count = &semaphores_[m.Arg(0)];
                   const auto take = [count, &m]
                   {
                       m.Return((*count)--);
                   };
                   if (static_cast<int32_t>(*count) > 0)
                   {
                       take();
                       return;
                   }
                   m.Wait([count] { return static_cast<int32_t>(*count) > 0; }, take);
               });
    m.Register("coreinit", "OSSignalSemaphore",
               [this](Machine& m)
               {
                   m.Return(semaphores_[m.Arg(0)]++);
                   m.Yield();
               });
    m.Register("coreinit", "OSGetSemaphoreCount", [this](Machine& m) { m.Return(semaphores_[m.Arg(0)]); });

    // OSInitEvent(event, signalled, mode): mode 1 resets the event as one waiter takes it.
    m.Register("coreinit", "OSInitEventEx",
               [this](Machine& m) { events_[m.Arg(0)] = {m.Arg(1) != 0, m.Arg(2) == kEventAutoReset}; });
    m.Register("coreinit", "OSInitEvent",
               [this](Machine& m) { events_[m.Arg(0)] = {m.Arg(1) != 0, m.Arg(2) == kEventAutoReset}; });
    m.Register("coreinit", "OSSignalEvent",
               [this](Machine& m)
               {
                   events_[m.Arg(0)].signalled = true;
                   m.Yield();
               });
    m.Register("coreinit", "OSResetEvent", [this](Machine& m) { events_[m.Arg(0)].signalled = false; });
    m.Register("coreinit", "OSWaitEvent",
               [this](Machine& m)
               {
                   Event* const event = &events_[m.Arg(0)];
                   const auto take = [event]
                   {
                       if (event->auto_reset)
                       {
                           event->signalled = false;
                       }
                   };
                   if (event->signalled)
                   {
                       take();
                       return;
                   }
                   m.Wait([event] { return event->signalled; }, take);
               });
    // OSWaitEventWithTimeout(event, timeout): true if it was signalled in time.
    m.Register("coreinit", "OSWaitEventWithTimeout",
               [this](Machine& m)
               {
                   Event* const event = &events_[m.Arg(0)];
                   const uint64_t deadline = time_ + Arg64(2);
                   const auto finish = [event, &m]
                   {
                       const bool signalled = event->signalled;
                       if (signalled && event->auto_reset)
                       {
                           event->signalled = false;
                       }
                       m.Return(signalled ? 1 : 0);
                   };
                   if (event->signalled || Arg64(2) == 0)
                   {
                       finish();
                       return;
                   }
                   m.Wait([this, event, deadline] { return event->signalled || time_ >= deadline; }, finish);
               });

    // OSInitMessageQueue(queue, array, count); OSSendMessage and OSReceiveMessage(queue, message, flags). Messages are
    // 16 bytes.
    m.Register("coreinit", "OSInitMessageQueue",
               [this](Machine& m)
               {
                   queues_[m.Arg(0)] = Queue();
                   queues_[m.Arg(0)].capacity = m.Arg(2);
               });
    m.Register("coreinit", "OSSendMessage",
               [this](Machine& m)
               {
                   Queue* const queue = &queues_[m.Arg(0)];
                   std::array<uint8_t, 16> message{};
                   std::memcpy(message.data(), m.memory.Pointer(m.Arg(1), 16), 16);
                   const bool front = (m.Arg(2) & kMessageHighPriority) != 0;
                   const auto send = [queue, message, front, &m]
                   {
                       if (front)
                       {
                           queue->messages.push_front(message);
                       }
                       else
                       {
                           queue->messages.push_back(message);
                       }
                       m.Return(1);
                   };
                   if (queue->messages.size() < queue->capacity)
                   {
                       send();
                       m.Yield();
                       return;
                   }
                   if (!(m.Arg(2) & kMessageBlock))
                   {
                       m.Return(0);
                       return;
                   }
                   m.Wait([queue] { return queue->messages.size() < queue->capacity; }, send);
               });
    m.Register("coreinit", "OSReceiveMessage",
               [this](Machine& m)
               {
                   Queue* const queue = &queues_[m.Arg(0)];
                   const uint32_t out = m.Arg(1);
                   const auto receive = [queue, out, &m]
                   {
                       std::memcpy(m.memory.Pointer(out, 16), queue->messages.front().data(), 16);
                       queue->messages.pop_front();
                       m.Return(1);
                   };
                   if (!queue->messages.empty())
                   {
                       receive();
                       return;
                   }
                   if (!(m.Arg(2) & kMessageBlock))
                   {
                       m.Return(0);
                       return;
                   }
                   m.Wait([queue] { return !queue->messages.empty(); }, receive);
               });

    // Alarms: OSSetAlarm(alarm, delay, callback) and OSSetPeriodicAlarm(alarm, start, period, callback). A periodic
    // alarm's start is a time, not a delay. The alarms' callbacks run as time passes.
    m.Register("coreinit", "OSCreateAlarm", [this](Machine& m) { alarms_[m.Arg(0)] = Alarm(); });
    m.Register("coreinit", "OSSetAlarm",
               [this](Machine& m)
               {
                   Alarm& alarm = alarms_[m.Arg(0)];
                   alarm = {true, time_ + Arg64(2), 0, m.Arg(4), alarm.user_data, alarm_order_++};
                   m.Return(1);
               });
    m.Register("coreinit", "OSSetPeriodicAlarm",
               [this](Machine& m)
               {
                   Alarm& alarm = alarms_[m.Arg(0)];
                   alarm = {true, Arg64(2), Arg64(4), m.Arg(6), alarm.user_data, alarm_order_++};
                   if (alarm.period != 0)
                   {
                       while (alarm.due <= time_)
                       {
                           alarm.due += alarm.period;
                       }
                   }
                   m.Return(1);
               });
    m.Register("coreinit", "OSCancelAlarm",
               [this](Machine& m)
               {
                   alarms_[m.Arg(0)].set = false;
                   m.Return(1);
               });
    m.Register("coreinit", "OSSetAlarmUserData", [this](Machine& m) { alarms_[m.Arg(0)].user_data = m.Arg(1); });
    m.Register("coreinit", "OSGetAlarmUserData", [this](Machine& m) { m.Return(alarms_[m.Arg(0)].user_data); });
}

void Coreinit::RegisterTime()
{
    Machine& m = machine_;

    // The time since the system started, and the time of day. The time of day starts at 2000-01-01 here.
    m.Register("coreinit", "OSGetTime", [this](Machine&) { Return64(time_); });
    m.Register("coreinit", "OSGetSystemTime", [this](Machine&) { Return64(time_); });
    m.Register("coreinit", "OSGetTick", [this](Machine& m) { m.Return(static_cast<uint32_t>(time_)); });
    m.Register("coreinit", "OSGetSystemTick", [this](Machine& m) { m.Return(static_cast<uint32_t>(time_)); });
    m.Register("coreinit", "OSGetSystemInfo", [this](Machine& m) { m.Return(system_info_); });
}

void Coreinit::RegisterMemory()
{
    Machine& m = machine_;

    const auto fill = [](Machine& m, uint32_t address, uint8_t value, uint32_t size)
    {
        if (size != 0)
        {
            std::memset(m.memory.Pointer(address, size), value, size);
        }
    };
    const auto move = [](Machine& m, uint32_t to, uint32_t from, uint32_t size)
    {
        if (size != 0)
        {
            std::memmove(m.memory.Pointer(to, size), m.memory.Pointer(from, size), size);
        }
    };
    m.Register("coreinit", "memcpy", [move](Machine& m) { move(m, m.Arg(0), m.Arg(1), m.Arg(2)); });
    m.Register("coreinit", "memmove", [move](Machine& m) { move(m, m.Arg(0), m.Arg(1), m.Arg(2)); });
    m.Register("coreinit", "memset",
               [fill](Machine& m) { fill(m, m.Arg(0), static_cast<uint8_t>(m.Arg(1)), m.Arg(2)); });
    m.Register("coreinit", "OSBlockMove", [move](Machine& m) { move(m, m.Arg(0), m.Arg(1), m.Arg(2)); });
    m.Register("coreinit", "OSBlockSet",
               [fill](Machine& m) { fill(m, m.Arg(0), static_cast<uint8_t>(m.Arg(1)), m.Arg(2)); });
    m.Register("coreinit", "DCZeroRange", [fill](Machine& m) { fill(m, m.Arg(0), 0, m.Arg(1)); });

    // The host handles the caches and the memory order: nothing to do.
    for (const char* name : {"DCFlushRange", "DCFlushRangeNoSync", "DCInvalidateRange", "DCStoreRange",
                             "DCStoreRangeNoSync", "OSCoherencyBarrier", "OSMemoryBarrier", "OSEnforceInorderIO"})
    {
        m.Register("coreinit", name, [](Machine&) {});
    }
    m.Register("coreinit", "OSEffectiveToPhysical", [](Machine& m) { m.Return(m.Arg(0)); });

    // Frame heaps: allocations from the head (a positive alignment) or the tail (a negative one). The allocations are
    // freed together.
    m.Register("coreinit", "MEMCreateFrmHeapEx",
               [this](Machine& m)
               {
                   const uint32_t start = AlignUp(m.Arg(0), 4);
                   const uint32_t end = AlignDown(m.Arg(0) + m.Arg(1), 4);
                   if (end < start || end - start < kFrameHeapHeader)
                   {
                       m.Return(0);
                       return;
                   }
                   frame_heaps_[start] = {start, end, start + kFrameHeapHeader, end};
                   m.Return(start);
               });
    m.Register("coreinit", "MEMDestroyFrmHeap",
               [this](Machine& m)
               {
                   frame_heaps_.erase(m.Arg(0));
                   m.Return(m.Arg(0));
               });
    m.Register("coreinit", "MEMAllocFromFrmHeapEx",
               [this](Machine& m)
               {
                   const auto heap = frame_heaps_.find(m.Arg(0));
                   if (heap == frame_heaps_.end())
                   {
                       m.Return(0);
                       return;
                   }
                   FrameHeap& h = heap->second;
                   const uint32_t size = m.Arg(1);
                   const auto align = static_cast<int32_t>(m.Arg(2));
                   if (align >= 0)
                   {
                       const uint32_t start = AlignUp(h.head, static_cast<uint32_t>(std::max(align, 4)));
                       if (start > h.tail || size > h.tail - start)
                       {
                           m.Return(0);
                           return;
                       }
                       h.head = start + size;
                       m.Return(start);
                   }
                   else
                   {
                       if (size > h.tail - h.head)
                       {
                           m.Return(0);
                           return;
                       }
                       const uint32_t start = AlignDown(h.tail - size, static_cast<uint32_t>(-align));
                       if (start < h.head)
                       {
                           m.Return(0);
                           return;
                       }
                       h.tail = start;
                       m.Return(start);
                   }
               });
    m.Register("coreinit", "MEMFreeToFrmHeap",
               [this](Machine& m)
               {
                   const auto heap = frame_heaps_.find(m.Arg(0));
                   if (heap == frame_heaps_.end())
                   {
                       return;
                   }
                   if (m.Arg(1) & kFreeHead)
                   {
                       heap->second.head = heap->second.start + kFrameHeapHeader;
                   }
                   if (m.Arg(1) & kFreeTail)
                   {
                       heap->second.tail = heap->second.end;
                   }
               });
}

void Coreinit::RegisterHeaps()
{
    Machine& m = machine_;

    // Expanded heaps: MEMCreateExpHeapEx(address, size, flags); allocations from the head (a positive alignment) or
    // the tail (a negative one).
    m.Register("coreinit", "MEMCreateExpHeapEx",
               [this](Machine& m)
               {
                   const uint32_t start = AlignUp(m.Arg(0), 4);
                   const uint32_t end = AlignDown(m.Arg(0) + m.Arg(1), 4);
                   if (end <= start + kFrameHeapHeader)
                   {
                       m.Return(0);
                       return;
                   }
                   ExpHeap& heap = exp_heaps_[start];
                   heap = ExpHeap();
                   heap.start = start;
                   heap.end = end;
                   heap.free[start + kFrameHeapHeader] = end - (start + kFrameHeapHeader);
                   m.Return(start);
               });
    m.Register("coreinit", "MEMDestroyExpHeap",
               [this](Machine& m)
               {
                   exp_heaps_.erase(m.Arg(0));
                   m.Return(m.Arg(0));
               });
    m.Register("coreinit", "MEMAllocFromExpHeapEx",
               [this](Machine& m)
               {
                   const auto heap = exp_heaps_.find(m.Arg(0));
                   const auto align = static_cast<int32_t>(m.Arg(2));
                   m.Return(heap == exp_heaps_.end()
                                ? 0
                                : heap->second.Allocate(m.Arg(1), static_cast<uint32_t>(align < 0 ? -align : align),
                                                        align < 0));
               });
    m.Register("coreinit", "MEMFreeToExpHeap",
               [this](Machine& m)
               {
                   const auto heap = exp_heaps_.find(m.Arg(0));
                   if (heap != exp_heaps_.end())
                   {
                       heap->second.Release(m.Arg(1));
                   }
               });
    m.Register("coreinit", "MEMGetSizeForMBlockExpHeap",
               [this](Machine& m)
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
               });
    m.Register("coreinit", "MEMGetTotalFreeSizeForExpHeap",
               [this](Machine& m)
               {
                   uint32_t total = 0;
                   if (const auto heap = exp_heaps_.find(m.Arg(0)); heap != exp_heaps_.end())
                   {
                       for (const auto& [address, size] : heap->second.free)
                       {
                           total += size;
                       }
                   }
                   m.Return(total);
               });
    m.Register("coreinit", "MEMGetAllocatableSizeForExpHeapEx",
               [this](Machine& m)
               {
                   uint32_t largest = 0;
                   const auto align = static_cast<int32_t>(m.Arg(1));
                   const uint32_t a = std::max<uint32_t>(static_cast<uint32_t>(align < 0 ? -align : align), 4);
                   if (const auto heap = exp_heaps_.find(m.Arg(0)); heap != exp_heaps_.end())
                   {
                       for (const auto& [address, size] : heap->second.free)
                       {
                           const uint32_t lost = AlignUp(address, a) - address;
                           largest = std::max(largest, size > lost ? AlignDown(size - lost, 4) : 0u);
                       }
                   }
                   m.Return(largest);
               });
    m.Register("coreinit", "MEMGetAllocatableSizeForFrmHeapEx",
               [this](Machine& m)
               {
                   const auto heap = frame_heaps_.find(m.Arg(0));
                   if (heap == frame_heaps_.end())
                   {
                       m.Return(0);
                       return;
                   }
                   const auto align = static_cast<int32_t>(m.Arg(1));
                   const uint32_t a = std::max<uint32_t>(static_cast<uint32_t>(align < 0 ? -align : align), 4);
                   const uint32_t start = AlignUp(heap->second.head, a);
                   m.Return(start < heap->second.tail ? heap->second.tail - start : 0);
               });
    m.Register("coreinit", "MEMFindContainHeap",
               [this](Machine& m)
               {
                   for (const auto& [handle, heap] : exp_heaps_)
                   {
                       if (m.Arg(0) >= heap.start && m.Arg(0) < heap.end)
                       {
                           m.Return(handle);
                           return;
                       }
                   }
                   for (const auto& [handle, heap] : frame_heaps_)
                   {
                       if (m.Arg(0) >= heap.start && m.Arg(0) < heap.end)
                       {
                           m.Return(handle);
                           return;
                       }
                   }
                   m.Return(0);
               });
}

void Coreinit::RegisterRuntime()
{
    Machine& m = machine_;

    // The C library's per-file and other locks. They can't be contended while the threads take turns on one core.
    for (const char* name : {"__ghs_mtx_init", "__ghs_mtx_lock", "__ghs_mtx_unlock", "__ghs_mtx_dst",
                             "__ghs_flock_create", "__ghs_flock_destroy", "__ghs_flock_file", "__ghs_funlock_file"})
    {
        m.Register("coreinit", name, [](Machine&) {});
    }
    m.Register("coreinit", "__ghs_ftrylock_file", [](Machine& m) { m.Return(0); });
    m.Register("coreinit", "__ghs_flock_ptr",
               [this](Machine& m)
               {
                   if (file_lock_ == 0)
                   {
                       file_lock_ = Alloc(16);
                   }
                   m.Return(file_lock_);
               });
    m.Register("coreinit", "__gh_errno_ptr",
               [this](Machine& m)
               {
                   if (errno_ == 0)
                   {
                       errno_ = Alloc(4);
                   }
                   m.Return(errno_);
               });
    m.Register("coreinit", "__gh_set_errno",
               [this](Machine& m)
               {
                   if (errno_ == 0)
                   {
                       errno_ = Alloc(4);
                   }
                   m.memory.Write32(errno_, m.Arg(0));
               });
    m.Register(
        "coreinit", "exit", [](Machine& m)
        { throw std::runtime_error("the guest called exit(" + std::to_string(static_cast<int32_t>(m.Arg(0))) + ")"); });
    m.Register(
        "coreinit", "_Exit",
        [](Machine& m)
        {
            throw std::runtime_error("the guest called _Exit(" + std::to_string(static_cast<int32_t>(m.Arg(0))) + ")");
        });

    // Interrupts and the debugger. Nothing raises interrupts here, and there's no debugger.
    m.Register("coreinit", "OSDisableInterrupts", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSRestoreInterrupts", [](Machine& m) { m.Return(1); });
    m.Register("coreinit", "OSIsDebuggerInitialized", [](Machine& m) { m.Return(0); });
    m.Register("coreinit", "OSIsDebuggerPresent", [](Machine& m) { m.Return(0); });
}

void Coreinit::RegisterFiles()
{
    Machine& m = machine_;

    m.Register("coreinit", "FSInitCmdBlock", [](Machine&) {});
    m.Register("coreinit", "FSSetCmdPriority", [](Machine& m) { m.Return(kFsOk); });
    m.Register("coreinit", "FSGetCwd",
               [](Machine& m)
               {
                   const std::string cwd = "/vol/content";
                   if (m.Arg(3) > cwd.size())
                   {
                       std::memcpy(m.memory.Pointer(m.Arg(2), static_cast<uint32_t>(cwd.size() + 1)), cwd.c_str(),
                                   cwd.size() + 1);
                   }
                   m.Return(kFsOk);
               });
    // FSOpenFile(client, block, path, mode, handle out, errors).
    m.Register("coreinit", "FSOpenFile",
               [this](Machine& m)
               {
                   const auto file = files_.find(m.memory.ReadString(m.Arg(2)));
                   if (file == files_.end())
                   {
                       m.Return(kFsNotFound);
                       return;
                   }
                   const uint32_t handle = next_file_++;
                   open_files_[handle] = {&file->second, 0};
                   m.memory.Write32(m.Arg(4), handle);
                   m.Return(kFsOk);
               });
    m.Register("coreinit", "FSCloseFile",
               [this](Machine& m)
               {
                   open_files_.erase(m.Arg(2));
                   m.Return(kFsOk);
               });
    // FSReadFile(client, block, buffer, size, count, handle, flags, errors): the number of whole elements read.
    m.Register("coreinit", "FSReadFile",
               [this](Machine& m)
               {
                   const auto file = open_files_.find(m.Arg(5));
                   const uint32_t size = m.Arg(3);
                   if (file == open_files_.end() || size == 0)
                   {
                       m.Return(kFsNotFound);
                       return;
                   }
                   OpenFile& f = file->second;
                   const uint32_t left = static_cast<uint32_t>(f.bytes->size()) -
                                         std::min<uint32_t>(f.position, static_cast<uint32_t>(f.bytes->size()));
                   const uint32_t count = std::min(m.Arg(4), left / size);
                   if (count != 0)
                   {
                       std::memcpy(m.memory.Pointer(m.Arg(2), count * size), f.bytes->data() + f.position,
                                   count * size);
                   }
                   f.position += count * size;
                   m.Return(count);
               });
    m.Register("coreinit", "FSSetPosFile",
               [this](Machine& m)
               {
                   const auto file = open_files_.find(m.Arg(2));
                   if (file == open_files_.end())
                   {
                       m.Return(kFsNotFound);
                       return;
                   }
                   file->second.position = m.Arg(3);
                   m.Return(kFsOk);
               });
    // FSGetStatFile(client, block, handle, stat out, errors): the file's size is at +0x10 of the FSStat.
    m.Register("coreinit", "FSGetStatFile",
               [this](Machine& m)
               {
                   const auto file = open_files_.find(m.Arg(2));
                   if (file == open_files_.end())
                   {
                       m.Return(kFsNotFound);
                       return;
                   }
                   std::memset(m.memory.Pointer(m.Arg(3), kStatSize), 0, kStatSize);
                   m.memory.Write32(m.Arg(3) + 0x10, static_cast<uint32_t>(file->second.bytes->size()));
                   m.Return(kFsOk);
               });
}

} // namespace csf::cafe
