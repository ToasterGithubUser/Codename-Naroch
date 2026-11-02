#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef void (*destructor_t)(void*);

struct destructor_entry {
    destructor_t destructor;
    void* object;
    void* dso;
};

static destructor_entry destructor_entries[64];
static size_t destructor_count;

extern "C" {
void* __dso_handle = &__dso_handle;
}

extern "C" int __cxa_atexit(destructor_t destructor, void* object,
                             void* dso) {
    if (!destructor || destructor_count == 64) {
        return -1;
    }
    destructor_entries[destructor_count++] = {destructor, object, dso};
    return 0;
}

extern "C" void __cxa_finalize(void* dso) {
    while (destructor_count) {
        destructor_entry entry = destructor_entries[--destructor_count];
        if (!dso || entry.dso == dso) {
            entry.destructor(entry.object);
        }
    }
}

extern "C" void __cxa_pure_virtual(void) {
    abort();
}

extern "C" int __cxa_guard_acquire(uint64_t* guard) {
    return (*(uint8_t*)guard == 0);
}

extern "C" void __cxa_guard_release(uint64_t* guard) {
    *(uint8_t*)guard = 1;
}

extern "C" void __cxa_guard_abort(uint64_t*) {}

void* operator new(size_t size) {
    void* memory = malloc(size);
    if (!memory) {
        abort();
    }
    return memory;
}

void* operator new[](size_t size) {
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept {
    free(pointer);
}

void operator delete[](void* pointer) noexcept {
    free(pointer);
}

void operator delete(void* pointer, size_t) noexcept {
    free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept {
    free(pointer);
}
