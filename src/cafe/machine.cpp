// SPDX-License-Identifier: MIT

#include "cafe/machine.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace csf::cafe
{
namespace
{

// The trap: primary opcode 0. The CPU has no such instruction.
constexpr uint32_t kTrapOpcode = 0x00CAFE00;

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kTrapArea = 0x10000;
constexpr uint32_t kMinStack = 0x100000;
constexpr uint32_t kDataImportSize = 0x400;
constexpr uint32_t kInterruptStack = 0x10000;

// HID2's bits that enable paired singles and their quantised loads and stores.
constexpr uint32_t kHid2PairedSingles = 0xA0000000;

uint32_t AlignUp(uint32_t value, uint32_t align)
{
    return (value + align - 1) & ~(align - 1);
}

std::string Hex(uint32_t value)
{
    static const char kDigits[] = "0123456789abcdef";
    std::string text = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        text += kDigits[(value >> shift) & 0xF];
    }

    return text;
}

// Fills in the field at `field` of a relocation whose symbol and addend come to `value`.
void Relocate(Memory& memory, uint32_t type, uint32_t field, uint32_t value)
{
    switch (type)
    {
    case Rpl::kAddr32:
        memory.Write32(field, value);
        break;
    case Rpl::kAddr16Lo:
        memory.Write16(field, static_cast<uint16_t>(value));
        break;
    case Rpl::kAddr16Hi:
        memory.Write16(field, static_cast<uint16_t>(value >> 16));
        break;
    case Rpl::kAddr16Ha:
        memory.Write16(field, static_cast<uint16_t>((value + 0x8000) >> 16));
        break;
    case Rpl::kRel24:
    case Rpl::kRel14:
        {
            const int64_t delta = int64_t{value} - field;
            const int64_t reach = type == Rpl::kRel24 ? 0x2000000 : 0x8000;
            const uint32_t mask = type == Rpl::kRel24 ? 0x03FFFFFC : 0xFFFC;
            if (delta < -reach || delta >= reach || (delta & 3) != 0)
            {
                throw std::runtime_error("a branch at " + Hex(field) + " can't reach " + Hex(value));
            }
            memory.Write32(field, (memory.Read32(field) & ~mask) | (static_cast<uint32_t>(delta) & mask));
            break;
        }
    case Rpl::kRel32:
        memory.Write32(field, value - field);
        break;
    default:
        throw std::runtime_error("a relocation of a type that game mode doesn't support (" + std::to_string(type) +
                                 ") at " + Hex(field));
    }
}

} // namespace

Machine::Machine(uint32_t heap_size) : heap_size_(heap_size)
{
}

void Machine::Register(const std::string& module, const std::string& name, Handler handler)
{
    handlers_[{module, name}] = std::move(handler);
}

void Machine::LoadExecutable(const Rpl& rpx)
{
    // The sections in the guest's memory: the allocated ones, below the loader's space at 0xC0000000.
    const auto placed = [](const Rpl::Section& section)
    {
        return (section.flags & Rpl::kAlloc) && section.address < 0xC0000000 && section.size != 0 &&
               (section.type == Rpl::kProgBits || section.type == Rpl::kNoBits);
    };
    uint32_t code_end = kCodeBase;
    uint32_t data_end = kDataBase;
    for (const Rpl::Section& section : rpx.sections)
    {
        if (!placed(section))
        {
            continue;
        }
        if (section.address < kCodeBase || section.address > 0xC0000000 - section.size)
        {
            throw std::runtime_error("the section " + section.name + " lies outside the code and data areas");
        }
        uint32_t& end = section.address < kDataBase ? code_end : data_end;
        end = std::max(end, section.address + section.size);
    }

    // The code, then the traps; the data, then the heap, then the stacks of calls from `idle` and of the main
    // thread.
    trap_next_ = AlignUp(code_end, kPage);
    trap_end_ = trap_next_ + kTrapArea;
    memory.AddRegion(kCodeBase, trap_end_ - kCodeBase);
    heap_next_ = AlignUp(data_end, kPage);
    heap_end_ = heap_next_ + AlignUp(heap_size_, kPage);
    interrupt_stack_top_ = heap_end_ + kInterruptStack;
    stack_top_ = interrupt_stack_top_ + AlignUp(std::max(rpx.stack_size, kMinStack), kPage);
    memory.AddRegion(kDataBase, stack_top_ - kDataBase);
    for (const Rpl::Section& section : rpx.sections)
    {
        if (placed(section) && section.type == Rpl::kProgBits)
        {
            std::copy(section.data.begin(), section.data.end(), memory.Pointer(section.address, section.size));
        }
    }
    return_trap_ = MakeTrap("the host's call", nullptr);

    // Each import's address: functions go to traps, and data to zeroed memory. The file doesn't give data imports'
    // sizes. Each data import gets 1 KiB. That's more than the C library's variables take up.
    std::vector<uint32_t> import_address(rpx.symbols.size(), 0);
    for (const Rpl::Import& import : rpx.imports)
    {
        uint32_t address = 0;
        if (import.data)
        {
            address = Allocate(kDataImportSize, 16);
            data_imports_[{import.module, import.name}] = address;
        }
        else
        {
            const auto handler = handlers_.find({import.module, import.name});
            address =
                MakeTrap(import.module + "." + import.name, handler != handlers_.end() ? handler->second : Handler());
        }
        import_address[import.symbol] = address;
    }

    // The relocations of the placed sections, with imports at their addresses. Those of undefined symbols are left
    // as the linker left them: the C library's start-up code checks such a symbol's address for 0 before calling it.
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (const Rpl::Section& section : rpx.sections)
    {
        if (placed(section))
        {
            ranges.push_back({section.address, section.address + section.size});
        }
    }
    for (const Rpl::Relocation& relocation : rpx.relocations)
    {
        const bool in_memory =
            std::any_of(ranges.begin(), ranges.end(), [&](const auto& range)
                        { return relocation.address >= range.first && relocation.address < range.second; });
        if (!in_memory || (rpx.symbols[relocation.symbol].section == 0 && !import_address[relocation.symbol]))
        {
            continue;
        }
        const uint32_t symbol = import_address[relocation.symbol] ? import_address[relocation.symbol]
                                                                  : rpx.symbols[relocation.symbol].value;
        Relocate(memory, relocation.type, relocation.address, symbol + static_cast<uint32_t>(relocation.addend));
    }

    // The CPU: memory mapped one to one by two 256 MiB blocks, one for the code and one for the data; floating point
    // and paired singles on; the small data areas' bases; and the quantisation registers Cafe OS gives every thread.
    ppc::State& s = cpu.state_;
    for (int i = 0; i < 2; ++i)
    {
        const uint32_t base = i == 0 ? 0x00000000 : 0x10000000;
        s.spr[ppc::spr::kIbat0u + i * 2] = base | 0x1FFF;
        s.spr[ppc::spr::kIbat0u + i * 2 + 1] = base | 0x2;
        s.spr[ppc::spr::kIbat0u + 8 + i * 2] = base | 0x1FFF;
        s.spr[ppc::spr::kIbat0u + 8 + i * 2 + 1] = base | 0x2;
    }
    s.spr[ppc::spr::kHid2] = kHid2PairedSingles;
    for (uint32_t i = 2; i <= 5; ++i)
    {
        s.spr[ppc::spr::kGqr0 + i] = (i + 2) << 16 | (i + 2);
    }
    s.msr = ppc::msr::kIr | ppc::msr::kDr | ppc::msr::kFp;
    s.gpr[2] = rpx.sda2_base;
    s.gpr[13] = rpx.sda_base;
    s.gpr[1] = stack_top_ - 64;
    cpu.InvalidateTranslation();
    initial_ = s;

    // The main thread, for the host's calls, and the trap that a thread's entry function returns to.
    threads_.clear();
    threads_.push_back(std::make_unique<Thread>());
    threads_.front()->name = "main";
    threads_.front()->started = true;
    current_ = threads_.front().get();
    exit_trap_ = MakeTrap("a thread's return", [](Machine& machine) { machine.ExitThread(machine.Arg(0)); });
}

uint32_t Machine::DataImport(const std::string& module, const std::string& name) const
{
    const auto it = data_imports_.find({module, name});

    return it != data_imports_.end() ? it->second : 0;
}

Machine::Result Machine::Call(uint32_t address, std::initializer_list<uint32_t> args,
                              std::initializer_list<double> fargs, uint64_t limit)
{
    // A call from the host runs on the main thread. The call starts from the registers that every thread starts from. A
    // call from a handler runs below the stack of the code that called the handler. A call from `idle` runs on a stack
    // of its own.
    const bool from_host = !host_call_ && nested_ == 0 && current_ == &MainThread();
    const bool interrupt = !from_host && current_ == nullptr && interrupts_ == 0;
    const ppc::State saved = cpu.state_;
    ppc::State& s = cpu.state_;
    if (from_host)
    {
        s = initial_;
    }
    int gpr = 3;
    for (const uint32_t arg : args)
    {
        s.gpr[gpr++] = arg;
    }
    int fpr = 1;
    for (const double arg : fargs)
    {
        s.fpr[fpr].ps0 = arg;
        s.fpr[fpr++].ps1 = arg;
    }
    const uint32_t top = from_host ? stack_top_ : interrupt ? interrupt_stack_top_ : saved.gpr[1];
    s.gpr[1] = (top - 256) & ~15u;
    memory.Write32(s.gpr[1], 0);
    s.lr = return_trap_;
    s.pc = address;

    if (from_host)
    {
        host_call_ = true;
    }
    else
    {
        ++nested_;
        interrupts_ += interrupt ? 1 : 0;
    }
    const auto finish = [&]
    {
        if (from_host)
        {
            host_call_ = false;
            current_ = &MainThread();
        }
        else
        {
            --nested_;
            interrupts_ -= interrupt ? 1 : 0;
        }
    };
    try
    {
        Run(limit);
    }
    catch (...)
    {
        finish();
        cpu.state_ = saved;
        throw;
    }
    finish();

    const Result result{s.gpr[3], s.gpr[4], s.fpr[1].ps0};
    cpu.state_ = saved;

    return result;
}

Machine::Result Machine::Interrupt(uint32_t address, std::initializer_list<uint32_t> args, uint64_t limit)
{
    Thread* const was = current_;
    current_ = nullptr;
    try
    {
        const Result result = Call(address, args, {}, limit);
        current_ = was;
        return result;
    }
    catch (...)
    {
        current_ = was;
        throw;
    }
}

void Machine::RunThreads(uint64_t limit)
{
    if (host_call_ || nested_ > 0)
    {
        throw std::runtime_error("RunThreads called from inside a call");
    }
    current_ = nullptr;
    Dispatch();
    if (current_ != nullptr)
    {
        try
        {
            Run(limit);
        }
        catch (...)
        {
            current_ = &MainThread();
            throw;
        }
    }
    current_ = &MainThread();
}

void Machine::Run(uint64_t limit)
{
    const uint64_t start = cpu.Cycles();
    ppc::State& s = cpu.state_;
    for (;;)
    {
        // Between calls from the host, the threads have run until none can continue.
        if (current_ == nullptr && nested_ == 0 && !host_call_)
        {
            return;
        }
        const uint64_t used = cpu.Cycles() - start;
        if (used >= limit)
        {
            throw std::runtime_error("the guest ran for too long, at " + Hex(s.pc));
        }
        const ppc::StopReason reason = cpu.Run(limit - used);
        if (reason == ppc::StopReason::kBudget)
        {
            continue;
        }
        if (reason == ppc::StopReason::kFault)
        {
            throw std::runtime_error("the guest reached unmapped memory at " + Hex(cpu.fault_address_) + " from " +
                                     Hex(cpu.stop_pc_));
        }

        const uint32_t pc = cpu.stop_pc_;
        const auto trap = traps_.find(pc);
        if (cpu.stop_opcode_ != kTrapOpcode || trap == traps_.end())
        {
            throw std::runtime_error("the guest ran an unknown instruction " + Hex(cpu.stop_opcode_) + " at " +
                                     Hex(pc));
        }

        // Only the innermost call's code returns here: other threads end at the exit trap, and calls from handlers
        // finish before their handlers return.
        if (pc == return_trap_)
        {
            return;
        }
        if (!trap->second.handler)
        {
            throw std::runtime_error("the guest called " + trap->second.name + ": game mode doesn't have it");
        }
        if (trace)
        {
            trace(trap->second.name);
        }
        switched_ = false;
        trap->second.handler(*this);
        if (!switched_)
        {
            s.pc = s.lr;
        }
        switched_ = false;
    }
}

Machine::Thread& Machine::CreateThread(std::string name, uint32_t object, uint32_t entry, uint32_t arg0, uint32_t arg1,
                                       uint32_t stack_top, int priority)
{
    auto thread = std::make_unique<Thread>();
    thread->name = std::move(name);
    thread->object = object;
    thread->priority = priority;
    thread->state = initial_;
    thread->state.pc = entry;
    thread->state.gpr[3] = arg0;
    thread->state.gpr[4] = arg1;
    thread->state.gpr[1] = (stack_top & ~15u) - 16;
    memory.Write32(thread->state.gpr[1], 0);
    thread->state.lr = exit_trap_;
    threads_.push_back(std::move(thread));

    return *threads_.back();
}

Machine::Thread* Machine::FindThread(uint32_t object)
{
    for (const auto& thread : threads_)
    {
        if (thread->object == object && object != 0)
        {
            return thread.get();
        }
    }

    return nullptr;
}

void Machine::Wait(std::function<bool()> until, std::function<void()> then)
{
    if (nested_ > 0 || current_ == nullptr)
    {
        throw std::runtime_error("a callback waited: game mode doesn't allow that");
    }
    Thread& thread = *current_;
    thread.until = std::move(until);
    thread.then = std::move(then);
    thread.state = cpu.state_;
    thread.state.pc = thread.state.lr;
    current_ = nullptr;
    switched_ = true;
    Dispatch();
}

void Machine::Yield()
{
    if (nested_ > 0 || current_ == nullptr)
    {
        return;
    }
    Thread& thread = *current_;
    const Thread* const next = Next();
    if (next == nullptr || next->priority >= thread.priority)
    {
        return;
    }
    thread.state = cpu.state_;
    thread.state.pc = thread.state.lr;
    current_ = nullptr;
    switched_ = true;
    Dispatch();
}

void Machine::ExitThread(uint32_t value)
{
    Thread* const thread = current_;
    if (thread == nullptr || thread == &MainThread() || nested_ > 0)
    {
        throw std::runtime_error("a thread ended out of turn: the main thread, or a callback");
    }
    thread->exited = true;
    thread->exit_value = value;
    current_ = nullptr;
    switched_ = true;
    Dispatch();
}

void Machine::Dispatch()
{
    for (;;)
    {
        if (Thread* const next = Next())
        {
            Switch(*next);
            return;
        }
        if (!host_call_)
        {
            current_ = nullptr;
            return;
        }
        if (!idle || !idle())
        {
            throw std::runtime_error("every guest thread is waiting, and nothing will wake them");
        }
    }
}

Machine::Thread* Machine::Next()
{
    Thread* best = nullptr;
    const size_t count = threads_.size();
    for (size_t step = 1; step <= count; ++step)
    {
        Thread& thread = *threads_[(last_ + step) % count];
        // Between the host's calls, the main thread belongs to the host.
        const bool host_has_it = &thread == threads_.front().get() && !host_call_;
        if (!thread.started || thread.exited || host_has_it || &thread == current_ || (thread.until && !thread.until()))
        {
            continue;
        }
        if (best == nullptr || thread.priority < best->priority)
        {
            best = &thread;
        }
    }

    return best;
}

void Machine::Switch(Thread& thread)
{
    current_ = &thread;
    for (size_t i = 0; i < threads_.size(); ++i)
    {
        if (threads_[i].get() == &thread)
        {
            last_ = i;
        }
    }
    cpu.state_ = thread.state;
    switched_ = true;
    if (thread.until)
    {
        thread.until = nullptr;
        if (std::function<void()> then = std::move(thread.then))
        {
            thread.then = nullptr;
            then();
        }
    }
}

uint32_t Machine::Allocate(uint32_t size, uint32_t align)
{
    const uint32_t address = AlignUp(heap_next_, align);
    if (address < heap_next_ || address > heap_end_ || size > heap_end_ - address)
    {
        throw std::runtime_error("game mode's heap is used up");
    }
    heap_next_ = address + size;

    return address;
}

uint32_t Machine::MakeTrap(const std::string& name, Handler handler)
{
    const uint32_t address = NextTrap();
    memory.Write32(address, kTrapOpcode);
    traps_[address] = {name, std::move(handler)};

    return address;
}

uint32_t Machine::NextTrap()
{
    if (trap_next_ >= trap_end_)
    {
        throw std::runtime_error("game mode has run out of traps");
    }
    const uint32_t address = trap_next_;
    trap_next_ += 4;

    return address;
}

} // namespace csf::cafe
