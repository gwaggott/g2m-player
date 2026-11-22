// Counts every allocation through operator new, to check real-time code
// makes none. Include in one file per test executable: it replaces the
// global operator new / delete.
#pragma once

#include <cstdlib>
#include <new>

static long allocations = 0;

void* operator new(std::size_t size) {
    ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
