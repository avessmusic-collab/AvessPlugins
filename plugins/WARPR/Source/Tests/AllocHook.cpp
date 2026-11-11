#include "Tests/AllocHook.h"

#include <cstdlib>
#include <new>

namespace
{
    thread_local bool        tArmed = false;
    thread_local std::size_t tCount = 0;

    void* countedAlloc (std::size_t n)
    {
        if (tArmed)
            ++tCount;

        if (void* p = std::malloc (n != 0 ? n : 1))
            return p;

        throw std::bad_alloc();
    }
}

namespace warpr::tests
{
    ScopedAllocCount::ScopedAllocCount()
    {
        startCount = tCount;
        tArmed = true;
    }

    ScopedAllocCount::~ScopedAllocCount()
    {
        tArmed = false;
    }

    std::size_t ScopedAllocCount::count() const
    {
        return tCount - startCount;
    }
}

// Global replacements (only linked into WARPR_Tests).  Aligned variants are left to the
// default implementation, which never routes through these.
void* operator new (std::size_t n)                    { return countedAlloc (n); }
void* operator new[] (std::size_t n)                  { return countedAlloc (n); }
void  operator delete (void* p) noexcept              { std::free (p); }
void  operator delete[] (void* p) noexcept            { std::free (p); }
void  operator delete (void* p, std::size_t) noexcept   { std::free (p); }
void  operator delete[] (void* p, std::size_t) noexcept { std::free (p); }
