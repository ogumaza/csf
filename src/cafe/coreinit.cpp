// SPDX-License-Identifier: MIT

#include "cafe/coreinit.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "xsflib/ascii.h"

#include "cafe/machine.h"
#include "cafe/memory.h"

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

// The bytes at the start of a frame heap or an expanded heap that the heap's header takes. No allocation uses them.
constexpr uint32_t kHeapHeader = 0x40;

constexpr uint32_t kStatSize = 0x64;

// The alignment that a heap function's signed alignment asks for: its magnitude. A negative alignment takes the block
// from the heap's tail.
uint32_t Alignment(int32_t align)
{
    return static_cast<uint32_t>(align < 0 ? -align : align);
}

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

Coreinit::Coreinit(Machine& machine) : machine_(machine)
{
    RegisterThreads();
    RegisterReports();
    RegisterLocks();
    RegisterSemaphores();
    RegisterEvents();
    RegisterMessageQueues();
    RegisterAlarms();
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

    const auto mem_alloc_from_default_heap_ex = [this](Machine& m)
    {
        const auto align = static_cast<int32_t>(m.Arg(1));
        m.Return(Alloc(m.Arg(0), std::max(Alignment(align), 4u)));
    };
    set("MEMAllocFromDefaultHeapEx", mem_alloc_from_default_heap_ex);

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
    // The console's file system ignores the case of ASCII letters. The table keeps the paths in lowercase.
    files_[xsflib::AsciiLower(path)] = std::move(bytes);
}

bool Coreinit::Mutex::TryLock(Machine::Thread* thread)
{
    if (owner != nullptr && owner != thread)
    {
        return false;
    }

    owner = thread;
    ++count;

    return true;
}

bool Coreinit::Event::Wakes(uint64_t seen) const
{
    return signalled || (!auto_reset && signals != seen);
}

void Coreinit::Event::Take()
{
    if (auto_reset)
    {
        signalled = false;
    }
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

const std::vector<uint8_t>* Coreinit::FindFile(const std::string& path) const
{
    const auto file = files_.find(xsflib::AsciiLower(path));
    return file != files_.end() ? &file->second : nullptr;
}

void Coreinit::RegisterThreads()
{
    Machine& machine = machine_;

    // OSCreateThread(thread, entry, argc, argv, stack top, stack size, priority, attributes).
    const auto os_create_thread = [](Machine& m)
    {
        m.CreateThread(m.Arg(0), m.Arg(1), m.Arg(2), m.Arg(3), m.Arg(4), static_cast<int>(m.Arg(6)));
        m.Return(1);
    };
    machine.Register("coreinit", "OSCreateThread", os_create_thread);

    const auto os_resume_thread = [this](Machine& m)
    {
        Machine::Thread& thread = ThreadAt(m.Arg(0));
        const bool was_suspended = !thread.started;
        thread.started = true;
        m.Return(was_suspended ? 1 : 0);
        m.Yield();
    };
    machine.Register("coreinit", "OSResumeThread", os_resume_thread);

    // A thread's 16 words of thread-specific data: OSSetThreadSpecific(slot, value) and OSGetThreadSpecific(slot).
    // JSystem keeps each thread's current heap in slot 0. A callback that runs as an interrupt has no thread, and reads
    // zeros.
    const auto os_set_thread_specific = [](Machine& m)
    {
        if (Machine::Thread* thread = m.Current(); thread != nullptr && m.Arg(0) < thread->specific.size())
        {
            thread->specific[m.Arg(0)] = m.Arg(1);
        }
    };
    machine.Register("coreinit", "OSSetThreadSpecific", os_set_thread_specific);

    const auto os_get_thread_specific = [](Machine& m)
    {
        const Machine::Thread* thread = m.Current();
        m.Return(thread != nullptr && m.Arg(0) < thread->specific.size() ? thread->specific[m.Arg(0)] : 0);
    };
    machine.Register("coreinit", "OSGetThreadSpecific", os_get_thread_specific);

    // A detached thread can't be joined. Nothing here frees a thread's resources when it ends, so detaching changes
    // nothing.
    machine.Register("coreinit", "OSDetachThread", [](Machine&) {});

    const auto os_join_thread = [this](Machine& m)
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
    };
    machine.Register("coreinit", "OSJoinThread", os_join_thread);

    machine.Register("coreinit", "OSExitThread", [](Machine& m) { m.ExitThread(m.Arg(0)); });

    const auto os_get_current_thread = [](Machine& m)
    {
        m.Return(m.Current() != nullptr ? m.Current()->object : m.MainThread().object);
    };
    machine.Register("coreinit", "OSGetCurrentThread", os_get_current_thread);

    // A thread's name is for debuggers. Game mode keeps none.
    machine.Register("coreinit", "OSSetThreadName", [](Machine&) {});

    const auto os_set_thread_priority = [this](Machine& m)
    {
        ThreadAt(m.Arg(0)).priority = static_cast<int>(m.Arg(1));
        m.Return(1);
        m.Yield();
    };
    machine.Register("coreinit", "OSSetThreadPriority", os_set_thread_priority);

    machine.Register("coreinit", "OSIsThreadTerminated", [this](Machine& m) { m.Return(ThreadAt(m.Arg(0)).exited); });

    machine.Register("coreinit", "OSGetThreadPriority",
                     [this](Machine& m) { m.Return(static_cast<uint32_t>(ThreadAt(m.Arg(0)).priority)); });

    // OSSleepTicks(ticks): the thread waits while that much time passes.
    const auto os_sleep_ticks = [this](Machine& m)
    {
        const uint64_t deadline = time_ + Arg64(0);
        m.Wait([this, deadline] { return time_ >= deadline; });
    };
    machine.Register("coreinit", "OSSleepTicks", os_sleep_ticks);

    machine.Register("coreinit", "OSYieldThread", [](Machine&) {});

    // The guest's threads all take turns on one core here: the main core. On a Wii U, that's core 1.
    machine.Register("coreinit", "OSGetCoreId", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSGetMainCoreId", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSIsMainCore", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSGetCoreCount", [](Machine& m) { m.Return(3); });
    machine.Register("coreinit", "OSSetThreadAffinity", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSGetThreadAffinity", [](Machine& m) { m.Return(7); });

    // A thread queue: OSSleepThread waits until the next OSWakeupThread.
    machine.Register("coreinit", "OSInitThreadQueue", [this](Machine& m) { sleep_queues_[m.Arg(0)] = 0; });

    const auto os_sleep_thread = [this](Machine& m)
    {
        uint64_t* const wakes = &sleep_queues_[m.Arg(0)];
        const uint64_t now = *wakes;
        m.Wait([wakes, now] { return *wakes != now; });
    };
    machine.Register("coreinit", "OSSleepThread", os_sleep_thread);

    const auto os_wakeup_thread = [this](Machine& m)
    {
        ++sleep_queues_[m.Arg(0)];
        m.Yield();
    };
    machine.Register("coreinit", "OSWakeupThread", os_wakeup_thread);
}

void Coreinit::RegisterReports()
{
    Machine& machine = machine_;

    // The guest's messages: reports are dropped. A panic or fatal error stops the guest with its message.
    for (const char* name : {"OSReport", "OSReportInfo", "OSReportWarn", "OSReportVerbose", "COSInfo", "COSWarn",
                             "COSError", "COSVerbose"})
    {
        machine.Register("coreinit", name, [](Machine&) {});
    }

    const auto os_panic = [](Machine& m)
    {
        throw std::runtime_error("the guest panicked: " + m.memory.ReadString(m.Arg(2)) + " (" +
                                 m.memory.ReadString(m.Arg(0)) + ")");
    };
    machine.Register("coreinit", "OSPanic", os_panic);

    const auto os_fatal = [](Machine& m)
    {
        throw std::runtime_error("the guest stopped: " + m.memory.ReadString(m.Arg(0)));
    };
    machine.Register("coreinit", "OSFatal", os_fatal);
}

void Coreinit::RegisterLocks()
{
    Machine& machine = machine_;

    // The C library's global lock. The lock can't be contended while the threads take turns on one core.
    machine.Register("coreinit", "__ghsLock", [](Machine&) {});
    machine.Register("coreinit", "__ghsUnlock", [](Machine&) {});

    const auto os_init_mutex = [this](Machine& m)
    {
        mutexes_[m.Arg(0)] = Mutex();
    };
    machine.Register("coreinit", "OSInitMutex", os_init_mutex);

    // When another thread holds the mutex, the caller waits until the mutex is free. The caller then locks it.
    const auto os_lock_mutex = [this](Machine& m)
    {
        Mutex* const mutex = &mutexes_[m.Arg(0)];
        if (!mutex->TryLock(m.Current()))
        {
            m.Wait([mutex] { return mutex->owner == nullptr; }, [mutex, &m] { mutex->TryLock(m.Current()); });
        }
    };
    machine.Register("coreinit", "OSLockMutex", os_lock_mutex);

    const auto os_unlock_mutex = [this](Machine& m)
    {
        Mutex& mutex = mutexes_[m.Arg(0)];
        if (mutex.owner == m.Current() && mutex.count != 0 && --mutex.count == 0)
        {
            mutex.owner = nullptr;
            m.Yield();
        }
    };
    machine.Register("coreinit", "OSUnlockMutex", os_unlock_mutex);

    const auto os_try_lock_mutex = [this](Machine& m)
    {
        m.Return(mutexes_[m.Arg(0)].TryLock(m.Current()) ? 1 : 0);
    };
    machine.Register("coreinit", "OSTryLockMutex", os_try_lock_mutex);

    // OSFastMutex: an OSMutex by another name, here.
    machine.Register("coreinit", "OSFastMutex_Init", os_init_mutex);
    machine.Register("coreinit", "OSFastMutex_Lock", os_lock_mutex);
    machine.Register("coreinit", "OSFastMutex_Unlock", os_unlock_mutex);

    // Spin locks can't be contended while the threads take turns on one core.
    machine.Register("coreinit", "OSInitSpinLock", [](Machine&) {});
    machine.Register("coreinit", "OSAcquireSpinLock", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSReleaseSpinLock", [](Machine& m) { m.Return(1); });
}

void Coreinit::RegisterSemaphores()
{
    Machine& machine = machine_;

    // Semaphores: OSInitSemaphore(semaphore, count); a wait takes one when there's one to take.
    machine.Register("coreinit", "OSInitSemaphore", [this](Machine& m) { semaphores_[m.Arg(0)] = m.Arg(1); });

    const auto os_wait_semaphore = [this](Machine& m)
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
    };
    machine.Register("coreinit", "OSWaitSemaphore", os_wait_semaphore);

    const auto os_signal_semaphore = [this](Machine& m)
    {
        m.Return(semaphores_[m.Arg(0)]++);
        m.Yield();
    };
    machine.Register("coreinit", "OSSignalSemaphore", os_signal_semaphore);

    machine.Register("coreinit", "OSGetSemaphoreCount", [this](Machine& m) { m.Return(semaphores_[m.Arg(0)]); });
}

void Coreinit::RegisterEvents()
{
    Machine& machine = machine_;

    // OSInitEvent(event, signalled, mode): mode 1 resets the event as one waiter takes it. OSInitEventEx takes the same
    // arguments first.
    const auto os_init_event = [this](Machine& m)
    {
        events_[m.Arg(0)] = {m.Arg(1) != 0, m.Arg(2) == kEventAutoReset};
    };
    machine.Register("coreinit", "OSInitEvent", os_init_event);
    machine.Register("coreinit", "OSInitEventEx", os_init_event);

    // OSSignalEvent wakes the threads that wait on a manual-reset event there and then. A reset after it doesn't hold
    // them back (a final-mix callback can signal an event and reset it within the frame).
    const auto os_signal_event = [this](Machine& m)
    {
        Event& event = events_[m.Arg(0)];
        event.signalled = true;
        ++event.signals;
        m.Yield();
    };
    machine.Register("coreinit", "OSSignalEvent", os_signal_event);

    machine.Register("coreinit", "OSResetEvent", [this](Machine& m) { events_[m.Arg(0)].signalled = false; });

    const auto os_wait_event = [this](Machine& m)
    {
        Event* const event = &events_[m.Arg(0)];
        if (event->signalled)
        {
            event->Take();
            return;
        }

        const uint64_t seen = event->signals;
        m.Wait([event, seen] { return event->Wakes(seen); }, [event] { event->Take(); });
    };
    machine.Register("coreinit", "OSWaitEvent", os_wait_event);

    // OSWaitEventWithTimeout(event, timeout): true if it was signalled in time.
    const auto os_wait_event_with_timeout = [this](Machine& m)
    {
        Event* const event = &events_[m.Arg(0)];
        const uint64_t deadline = time_ + Arg64(2);
        const uint64_t seen = event->signals;
        const auto finish = [event, seen, &m]
        {
            const bool woken = event->Wakes(seen);
            if (woken)
            {
                event->Take();
            }
            m.Return(woken ? 1 : 0);
        };

        if (event->signalled || Arg64(2) == 0)
        {
            finish();
            return;
        }
        m.Wait([this, event, seen, deadline] { return event->Wakes(seen) || time_ >= deadline; }, finish);
    };
    machine.Register("coreinit", "OSWaitEventWithTimeout", os_wait_event_with_timeout);
}

void Coreinit::RegisterMessageQueues()
{
    Machine& machine = machine_;

    // OSInitMessageQueue(queue, array, count); OSSendMessage and OSReceiveMessage(queue, message, flags). Messages are
    // 16 bytes.
    const auto os_init_message_queue = [this](Machine& m)
    {
        queues_[m.Arg(0)] = Queue();
        queues_[m.Arg(0)].capacity = m.Arg(2);
    };
    machine.Register("coreinit", "OSInitMessageQueue", os_init_message_queue);

    const auto os_send_message = [this](Machine& m)
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
    };
    machine.Register("coreinit", "OSSendMessage", os_send_message);

    const auto os_receive_message = [this](Machine& m)
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
    };
    machine.Register("coreinit", "OSReceiveMessage", os_receive_message);
}

void Coreinit::RegisterAlarms()
{
    Machine& machine = machine_;

    // Alarms: OSSetAlarm(alarm, delay, callback) and OSSetPeriodicAlarm(alarm, start, period, callback). A periodic
    // alarm's start is a time, not a delay. The alarms' callbacks run as time passes.
    machine.Register("coreinit", "OSCreateAlarm", [this](Machine& m) { alarms_[m.Arg(0)] = Alarm(); });

    const auto os_set_alarm = [this](Machine& m)
    {
        Alarm& alarm = alarms_[m.Arg(0)];
        alarm = {true, time_ + Arg64(2), 0, m.Arg(4), alarm.user_data, alarm_order_++};
        m.Return(1);
    };
    machine.Register("coreinit", "OSSetAlarm", os_set_alarm);

    const auto os_set_periodic_alarm = [this](Machine& m)
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
    };
    machine.Register("coreinit", "OSSetPeriodicAlarm", os_set_periodic_alarm);

    const auto os_cancel_alarm = [this](Machine& m)
    {
        alarms_[m.Arg(0)].set = false;
        m.Return(1);
    };
    machine.Register("coreinit", "OSCancelAlarm", os_cancel_alarm);

    machine.Register("coreinit", "OSSetAlarmUserData", [this](Machine& m) { alarms_[m.Arg(0)].user_data = m.Arg(1); });
    machine.Register("coreinit", "OSGetAlarmUserData", [this](Machine& m) { m.Return(alarms_[m.Arg(0)].user_data); });
}

void Coreinit::RegisterTime()
{
    Machine& machine = machine_;

    // The time since the system started, and the time of day. The time of day starts at 2000-01-01 here.
    machine.Register("coreinit", "OSGetTime", [this](Machine&) { Return64(time_); });
    machine.Register("coreinit", "OSGetSystemTime", [this](Machine&) { Return64(time_); });
    machine.Register("coreinit", "OSGetTick", [this](Machine& m) { m.Return(static_cast<uint32_t>(time_)); });
    machine.Register("coreinit", "OSGetSystemTick", [this](Machine& m) { m.Return(static_cast<uint32_t>(time_)); });
    machine.Register("coreinit", "OSGetSystemInfo", [this](Machine& m) { m.Return(system_info_); });
}

void Coreinit::RegisterMemory()
{
    Machine& machine = machine_;

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

    machine.Register("coreinit", "memcpy", [move](Machine& m) { move(m, m.Arg(0), m.Arg(1), m.Arg(2)); });
    machine.Register("coreinit", "memmove", [move](Machine& m) { move(m, m.Arg(0), m.Arg(1), m.Arg(2)); });
    machine.Register("coreinit", "memset",
                     [fill](Machine& m) { fill(m, m.Arg(0), static_cast<uint8_t>(m.Arg(1)), m.Arg(2)); });
    machine.Register("coreinit", "OSBlockMove", [move](Machine& m) { move(m, m.Arg(0), m.Arg(1), m.Arg(2)); });
    machine.Register("coreinit", "OSBlockSet",
                     [fill](Machine& m) { fill(m, m.Arg(0), static_cast<uint8_t>(m.Arg(1)), m.Arg(2)); });
    machine.Register("coreinit", "DCZeroRange", [fill](Machine& m) { fill(m, m.Arg(0), 0, m.Arg(1)); });

    // The host handles the caches and the memory order: nothing to do.
    for (const char* name : {"DCFlushRange", "DCFlushRangeNoSync", "DCInvalidateRange", "DCStoreRange",
                             "DCStoreRangeNoSync", "OSCoherencyBarrier", "OSMemoryBarrier", "OSEnforceInorderIO"})
    {
        machine.Register("coreinit", name, [](Machine&) {});
    }

    machine.Register("coreinit", "OSEffectiveToPhysical", [](Machine& m) { m.Return(m.Arg(0)); });
}

void Coreinit::RegisterHeaps()
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

    const auto mem_find_contain_heap = [this](Machine& m)
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
    };
    machine.Register("coreinit", "MEMFindContainHeap", mem_find_contain_heap);

    // MEMGetBaseHeapHandle(arena): the default heap is MEM2's base heap (arena 1). Game mode has no base heap in the
    // other arenas, MEM1's and the foreground's. A JSystem game on sead's framework (The Wind Waker HD) asks for it as
    // it starts up.
    machine.Register("coreinit", "MEMGetBaseHeapHandle",
                     [this](Machine& m) { m.Return(m.Arg(0) == 1 ? default_heap_ : 0); });
}

void Coreinit::RegisterRuntime()
{
    Machine& machine = machine_;

    // The C library's per-file and other locks. They can't be contended while the threads take turns on one core.
    for (const char* name : {"__ghs_mtx_init", "__ghs_mtx_lock", "__ghs_mtx_unlock", "__ghs_mtx_dst",
                             "__ghs_flock_create", "__ghs_flock_destroy", "__ghs_flock_file", "__ghs_funlock_file"})
    {
        machine.Register("coreinit", name, [](Machine&) {});
    }
    machine.Register("coreinit", "__ghs_ftrylock_file", [](Machine& m) { m.Return(0); });

    const auto ghs_flock_ptr = [this](Machine& m)
    {
        if (file_lock_ == 0)
        {
            file_lock_ = Alloc(16);
        }
        m.Return(file_lock_);
    };
    machine.Register("coreinit", "__ghs_flock_ptr", ghs_flock_ptr);

    // The C library's errno, allocated at its first use.
    const auto errno_address = [this]
    {
        if (errno_ == 0)
        {
            errno_ = Alloc(4);
        }

        return errno_;
    };
    machine.Register("coreinit", "__gh_errno_ptr", [errno_address](Machine& m) { m.Return(errno_address()); });
    machine.Register("coreinit", "__gh_set_errno",
                     [errno_address](Machine& m) { m.memory.Write32(errno_address(), m.Arg(0)); });

    for (const char* name : {"exit", "_Exit"})
    {
        const auto handler = [name](Machine& m)
        {
            throw std::runtime_error(std::string("the guest called ") + name + "(" +
                                     std::to_string(static_cast<int32_t>(m.Arg(0))) + ")");
        };
        machine.Register("coreinit", name, handler);
    }

    // Interrupts and the debugger. Nothing raises interrupts here, and there's no debugger.
    machine.Register("coreinit", "OSDisableInterrupts", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSRestoreInterrupts", [](Machine& m) { m.Return(1); });
    machine.Register("coreinit", "OSIsDebuggerInitialized", [](Machine& m) { m.Return(0); });
    machine.Register("coreinit", "OSIsDebuggerPresent", [](Machine& m) { m.Return(0); });
}

void Coreinit::RegisterFiles()
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

    // FSOpenFile(client, block, path, mode, handle out, errors).
    const auto fs_open_file = [this](Machine& m)
    {
        const std::vector<uint8_t>* const bytes = FindFile(m.memory.ReadString(m.Arg(2)));
        if (bytes == nullptr)
        {
            m.Return(kFsNotFound);
            return;
        }

        const uint32_t handle = next_file_++;
        open_files_[handle] = {bytes, 0};
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
        const uint32_t count = ReadElements(m.memory, m.Arg(2), *f.bytes, f.position, size, m.Arg(4));
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

        m.Return(ReadElements(m.memory, m.Arg(2), *file->second.bytes, m.Arg(5), size, m.Arg(4)));
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

    // FSGetStat(client, block, path, stat out, errors): FSGetStatFile's answer for a file by its path.
    const auto fs_get_stat = [this](Machine& m)
    {
        const std::vector<uint8_t>* const bytes = FindFile(m.memory.ReadString(m.Arg(2)));
        if (bytes == nullptr)
        {
            m.Return(kFsNotFound);
            return;
        }

        WriteStat(m.memory, m.Arg(3), bytes->size());
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

        WriteStat(m.memory, m.Arg(3), file->second.bytes->size());
        m.Return(kFsOk);
    };
    machine.Register("coreinit", "FSGetStatFile", fs_get_stat_file);
}

} // namespace csf::cafe
