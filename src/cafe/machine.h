// SPDX-License-Identifier: MIT

// Game mode's Wii U: a game's executable in guest memory with the CPU to run it, and Cafe OS at a high level. Each
// function that the executable imports from the system's libraries is a trap: an illegal instruction at an address of
// its own near the code. The trap passes the call to a C++ handler. The handler reads its arguments from the CPU's
// registers and leaves its result there. The guest then continues at the return address. The host calls the guest's
// functions the same way round. A handler can also call back into the guest. AX does that to call a game's frame
// callbacks.
//
// The guest's threads take turns on the one CPU. A thread runs until it waits in a handler (for a message, an event or
// a mutex). Then the thread of the highest priority that can continue runs. Cafe OS would choose the same thread. When
// every thread is waiting, the machine calls the host's `idle` to make time pass. The driver's `idle` runs a frame of
// AX.

#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cafe/memory.h"
#include "cafe/rpl.h"
#include "ppc/cpu.h"

namespace csf::cafe
{

class Machine
{
public:
    // Where the executable's code and data go. The system's loader puts a game's code and data at the same addresses.
    static constexpr uint32_t kCodeBase = 0x02000000;
    static constexpr uint32_t kDataBase = 0x10000000;

    // What a guest function returned: r3 (and r4 for a 64-bit value) and f1.
    struct Result
    {
        uint32_t r3 = 0;
        uint32_t r4 = 0;
        double f1 = 0.0;
    };

    // A handler for an imported function.
    using Handler = std::function<void(Machine& machine)>;

    // A guest thread.
    struct Thread
    {
        std::string name;
        uint32_t object = 0; // the guest's OSThread
        int priority = 16;   // Cafe OS's priority: 0 runs first, 31 last
        bool started = false;
        bool exited = false;
        uint32_t exit_value = 0;
        ppc::State state;            // its registers while another thread runs
        std::function<bool()> until; // while it waits: true once it can continue
        std::function<void()> then;  // when it continues, with its registers in place: the result of its wait
    };

    // `heap_size` bytes of memory after the executable's data are the heap that Allocate takes from.
    explicit Machine(uint32_t heap_size);

    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;

    // Sets the handler of the imported function `name` from the library `module` (such as "coreinit"). When the guest
    // calls an import that has no handler, the guest stops with an error that names the import.
    void Register(const std::string& module, const std::string& name, Handler handler);

    // Places the executable's code and data at their addresses and resolves its imports: functions to traps, and data
    // to zeroed memory. Throws std::runtime_error if the executable can't be loaded.
    void LoadExecutable(const Rpl& rpx);

    // The guest address of the data import `name` from `module`, or 0.
    uint32_t DataImport(const std::string& module, const std::string& name) const;

    // Calls the guest function at `address` with integer arguments in r3 on and floating-point ones in f1 on. Returns
    // the function's result. From the host, the call runs on the main thread, and other threads run while it waits.
    // From a handler, or from `idle`, the call runs below the current stack and mustn't wait. When the call returns,
    // the CPU's registers are as they were before it. Throws std::runtime_error if the guest faults, runs an unknown
    // instruction, calls an import with no handler or runs for more than `limit` instructions.
    Result Call(uint32_t address, std::initializer_list<uint32_t> args = {}, std::initializer_list<double> fargs = {},
                uint64_t limit = 10'000'000'000);

    // Calls the guest function at `address` as an interrupt would: on a stack of its own, with no thread running. The
    // function can then wake threads but not wait. For the callbacks of AX's frames and of alarms.
    Result Interrupt(uint32_t address, std::initializer_list<uint32_t> args = {}, uint64_t limit = 10'000'000'000);

    // Runs the guest's threads, other than the main thread, until none can continue.
    void RunThreads(uint64_t limit = 10'000'000'000);

    // `size` bytes of zeroed guest memory, aligned to `align`, for the system's structures. Throws
    // std::runtime_error when the heap is used up.
    uint32_t Allocate(uint32_t size, uint32_t align = 16);

    // A trap that runs `handler` when the guest calls it. The system gives the guest such traps as function pointers.
    uint32_t MakeTrap(const std::string& name, Handler handler);

    // A thread, not yet started. The thread will run `entry` with `arg0` and `arg1` in r3 and r4, on the stack whose
    // top is `stack_top`. The thread exits with what `entry` returns.
    Thread& CreateThread(std::string name, uint32_t object, uint32_t entry, uint32_t arg0, uint32_t arg1,
                         uint32_t stack_top, int priority);

    // The thread whose OSThread is `object`, or nullptr.
    Thread* FindThread(uint32_t object);

    // The thread running now: the main thread outside the guest, and nullptr while `idle` runs.
    Thread* Current()
    {
        return current_;
    }

    Thread& MainThread()
    {
        return *threads_.front();
    }

    // From a handler: the current thread waits until `until` returns true. Then `then` runs with the thread's registers
    // in place, and gives the call its result. Other threads run meanwhile.
    void Wait(std::function<bool()> until, std::function<void()> then = {});

    // From a handler: gives way to a thread of a higher priority that can continue. Cafe OS would switch to that thread
    // at once.
    void Yield();

    // From a handler: the current thread ends with `value`. The effect is the same as when its entry function returns
    // `value`.
    void ExitThread(uint32_t value);

    // Called when every thread is waiting. The callback should let time pass until a thread can continue. If no thread
    // ever will, the callback returns false.
    std::function<bool()> idle;

    // For diagnostics: called with each import's name as the guest calls it.
    std::function<void(const std::string& name)> trace;

    // Every thread, the main thread first.
    const std::vector<std::unique_ptr<Thread>>& Threads() const
    {
        return threads_;
    }

    // The arguments and results of the call in a handler.
    uint32_t Arg(int n) const
    {
        return cpu.state_.gpr[3 + n];
    }

    double FloatArg(int n) const
    {
        return cpu.state_.fpr[1 + n].ps0;
    }

    void Return(uint32_t value)
    {
        cpu.state_.gpr[3] = value;
    }

    void ReturnFloat(double value)
    {
        cpu.state_.fpr[1].ps0 = value;
        cpu.state_.fpr[1].ps1 = value;
    }

    Memory memory;
    ppc::Cpu cpu{memory};

private:
    struct Trap
    {
        std::string name;
        Handler handler;
    };

    // The address of the next trap.
    uint32_t NextTrap();

    // Runs the CPU and serves traps until the innermost call returns to the host. Between calls, the CPU runs until no
    // thread can continue.
    void Run(uint64_t limit);

    // Passes the CPU to the thread that should run next. Time passes while no thread can run. Between calls from the
    // host, no thread is left running when none can run.
    void Dispatch();

    // The thread of the highest priority that can continue, or nullptr. Among equals, the next thread after the last
    // one to run comes first.
    Thread* Next();

    void Switch(Thread& thread);

    uint32_t heap_size_;
    uint32_t trap_next_ = 0;
    uint32_t trap_end_ = 0;
    uint32_t return_trap_ = 0;
    uint32_t exit_trap_ = 0;
    uint32_t heap_next_ = 0;
    uint32_t heap_end_ = 0;
    uint32_t stack_top_ = 0;
    uint32_t interrupt_stack_top_ = 0;
    std::map<std::pair<std::string, std::string>, Handler> handlers_;
    std::map<std::pair<std::string, std::string>, uint32_t> data_imports_;
    std::map<uint32_t, Trap> traps_;

    ppc::State initial_; // the registers every thread starts from
    std::vector<std::unique_ptr<Thread>> threads_;
    Thread* current_ = nullptr;
    size_t last_ = 0;        // the index of the thread that last ran
    bool host_call_ = false; // the host's call is running on the main thread
    int nested_ = 0;         // calls from handlers and from `idle`: they mustn't wait
    int interrupts_ = 0;     // calls from `idle`: they run on a stack of their own
    bool switched_ = false;  // a handler handed the CPU to another thread
};

} // namespace csf::cafe
