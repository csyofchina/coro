/*
 * Copyright (c) 2013 Matt Fichman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, APEXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "coro/Common.hpp"
#include "coro/Coroutine.hpp"
#include <iostream>

extern "C" {
coro::Coroutine* coroCurrent = coro::main().get();
}

void coroStart() throw() { coroCurrent->start(); }

#ifndef _WIN32
#include "Coroutine.Unix.cpp.inc"
#endif


namespace coro {

uint64_t pageRound(uint64_t addr, uint64_t multiple) {
// Rounds 'base' to the nearest 'multiple'
    return (addr/multiple)*multiple;
}

uint64_t pageSize() {
// Returns the system page size.
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize*8; 
#else
    return sysconf(_SC_PAGESIZE);
#endif
}

Stack::Stack(uint64_t size) : data_(0), size_(size) {
// Lazily initializes a large stack for the coroutine.  Initially, the
// coroutine stack is quite small, to lower memory usage.  As the stack grows,
// the Coroutine::fault handler will commit memory pages for the coroutine.
    if (size == 0) { return; }
#ifdef _WIN32
    data_ = (uint8_t*)VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
    if (!data_) {
        throw std::bad_alloc();
    }
#else
#ifdef __linux__
    data_ = (uint8_t*)mmap(0, size, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
#else
    data_ = (uint8_t*)mmap(0, size, PROT_NONE, MAP_ANON|MAP_PRIVATE, -1, 0);
#endif
    if (data_ == MAP_FAILED) {
        throw std::bad_alloc();
    }
#endif
}

Stack::~Stack() {
// Throw exception
    if (data_) {
#ifdef _WIN32
        VirtualFree((LPVOID)data_, size_, MEM_RELEASE); 
#else
        munmap(data_, size_); 
#endif
    }
}

Coroutine::Coroutine() : stack_(0) {
// Constructor for the main coroutine.
    status_ = Coroutine::RUNNING;
    stackPointer_ = 0;
}

Coroutine::~Coroutine() {
// Destroy the coroutine & clean up its stack
    if (!stack_.begin()) {
        // This is the main coroutine; don't free up anything, because we did
        // not allocate a stack for it.
    } else if (status_ != Coroutine::DEAD && status_ != Coroutine::NEW) {
        status_ = Coroutine::DELETED;
        swapContext(); // Allow the coroutine to clean up its stack
    }
}

void Coroutine::init(std::function<void()> const& func) {
// Creates a new coroutine and allocates a stack for it.
    coro::main();
    func_ = func;
    status_ = Coroutine::NEW;
    commit((uint64_t)stack_.end()-1); 
    // Commit the page at the top of the coroutine stack

#ifdef _WIN32
    assert((((uint8_t*)this)+8)==(uint8_t*)&stackPointer_);
#else
    assert((((uint8_t*)this)+16)==(uint8_t*)&stackPointer_);
#endif

#ifdef _WIN32
    struct StackFrame {
        void* rdi;
        void* rsi;
        void* rdx;
        void* rcx;
        void* rbx;
        void* rax;
        void* rbp;
        void* returnAddr; // coroStart() stack frame here
    };
#else
    struct StackFrame {
        void* r15;
        void* r14;
        void* r13;
        void* r12;
        void* r11;
        void* r10;
        void* r9;
        void* r8;
        void* rdi;
        void* rsi;
        void* rdx;
        void* rcx;
        void* rbx;
        void* rax;
        void* rbp;
        void* returnAddr;
        void* padding;
    }; 
#endif

    StackFrame frame;
    memset(&frame, 0, sizeof(frame));
    frame.returnAddr = (void*)coroStart;

    stackPointer_ = stack_.end();
    stackPointer_ -= sizeof(frame);
    memcpy(stackPointer_, &frame, sizeof(frame));
}

void Coroutine::swap() {
// Swaps control to this coroutine, causing the current coroutine to suspend.
// This function returns when another coroutine swaps back to this coroutine.
// If a coroutine dies it is asleep, then it throws an ExitException, which
// will cause the coroutine to exit.  If a coroutine is being deleted, and swap
// is called, then swap will throw an ExitException.
   // if(!coroCurrent->isMain() && coroCurrent->status_ == Coroutine::DELETED) {
  //      throw ExitException();
  //  } 
    swapContext(); 
}

void Coroutine::swapContext() {
// Swaps control to this coroutine, causing the current coroutine to suspend.
// This function returns when another coroutine swaps back to this coroutine.
// If a coroutine dies it is asleep, then it throws an ExitException, which
// will cause the coroutine to exit.
    Coroutine* current = coroCurrent;
    switch (status_) {
    case Coroutine::DELETED: break;
    case Coroutine::SUSPENDED: status_ = Coroutine::RUNNING; break;
    case Coroutine::NEW: status_ = Coroutine::RUNNING; break;
    case Coroutine::RUNNING: return; // already running
    case Coroutine::DEAD: // fallthrough
    default: assert(!"illegal state"); break;
    }
    switch (coroCurrent->status_) {
    case Coroutine::DEAD: break;
    case Coroutine::RUNNING: coroCurrent->status_ = Coroutine::SUSPENDED; break;
    case Coroutine::DELETED: break; // fallthrough
    case Coroutine::SUSPENDED: // fallthrough
    case Coroutine::NEW: // fallthrough
    default: assert(!"illegal state"); break;
    }
    coroCurrent = this;
    coroSwapContext(current, this);
    switch (coroCurrent->status_) {
    case Coroutine::DELETED: if (!coroCurrent->isMain()) { throw ExitException(); } break;
    case Coroutine::RUNNING: break; 
    case Coroutine::SUSPENDED: // fallthrough
    case Coroutine::NEW: // fallthrough
    case Coroutine::DEAD: // fallthrough
    default: assert(!"illegal state"); break;
    }
}

void Coroutine::start() throw() {
// This function runs the coroutine from the given entry point.
    try {
        func_();
        exit(); // OK, fell of the end of the coroutine function
    } catch(ExitException const& ex) {
        exit(); // OK, coroutine was deallocated
    } catch(...) {
        assert(!"error: coroutine killed by exception");
    }
}

void Coroutine::exit() {
// This function runs when the coroutine "falls of the stack," that is, when it finishes executing.
    assert(coroCurrent == this);
    switch (status_) {
    case Coroutine::DELETED: break;
    case Coroutine::RUNNING: status_ = Coroutine::DEAD; break;
    case Coroutine::DEAD: // fallthrough
    case Coroutine::SUSPENDED: // fallthrough
    case Coroutine::NEW: // fallthrough
    default: assert(!"illegal state"); break;
    }
    main()->swap();
}

void Coroutine::commit(uint64_t addr) {
// Ensures that 'addr' is allocated, and that the next page in the stack is a
// guard page.
    uint64_t psize = pageSize();
    uint64_t page = pageRound(addr, psize);
    uint64_t len = (uint64_t)stack_.end()-page;
    // Allocate all pages between stack_min and the current page.
#ifdef _WIN32
    uint64_t glen = psize;
    uint64_t guard = page-glen;
    assert(page < (uint64_t)stack_.end());
    if (!VirtualAlloc((LPVOID)page, len, MEM_COMMIT, PAGE_READWRITE)) {
        abort();     
    }
    // Create a guard page right after the current page.
    if (!VirtualAlloc((LPVOID)guard, glen, MEM_COMMIT, PAGE_READWRITE|PAGE_GUARD)) {
        abort();     
    }
#else
    assert(page < (uint64_t)stack_.end());
    assert(page >= (uint64_t)stack_.begin());
    if (mprotect((void*)page, len, PROT_READ|PROT_WRITE) == -1) {
        abort();
    }
#endif
}

Ptr<Coroutine> current() {
// Returns the coroutine that is currently executing.
    return coroCurrent->shared_from_this();
}

Ptr<Coroutine> main() {
// Returns the "main" coroutine (i.e., the main coroutine)
    static Ptr<Coroutine> main;
    if (!main) {
        main.reset(new Coroutine);
        registerSignalHandlers();
    }
    return main;
}

void yield() {
    main()->swap(); 
}


}