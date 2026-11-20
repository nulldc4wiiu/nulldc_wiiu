// Save states (features plan, phase 3; save_states=1, off until confirmed on a console). Each module that holds
// machine state lists it once, in a section that both saves and loads (wiiu/state_wiiu.cpp):
//
//     #include "wiiu/state_wiiu.h"
//     STATE_SECTION(gdrom)
//     {
//         S(read_buff);            // a variable or an array, by its size
//         SBUF(ram, 16 << 20);     // a buffer through a pointer
//         if (s.loading) ...       // what a load must recompute (handler pointers and the like)
//     }
//
// Sections are stored by name with their size, so a state from a build whose layout differs is refused, not loaded.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct StateIO
{
    uint8_t *buf;
    size_t pos, cap;
    bool loading;   // false: saving (or measuring, with buf null)
    bool ok;
    void io(void *p, size_t n);
};

typedef void (*StateFn)(StateIO &s);
struct StateSection
{
    const char *name;
    StateFn fn;
    StateSection *next;
    StateSection(const char *name, StateFn fn);
};

#define STATE_SECTION(name)                                                       \
    static void state_section_##name(StateIO &s);                                  \
    static StateSection state_section_reg_##name(#name, state_section_##name);     \
    static void state_section_##name(StateIO &s)
#define S(v) s.io((void *)&(v), sizeof(v))
#define SBUF(p, n) s.io((void *)(p), (n))
