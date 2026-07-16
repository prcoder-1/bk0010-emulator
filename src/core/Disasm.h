#pragma once
#include <cstdint>
#include <string>
#include "Memory.h"

namespace bk {

class SymbolTable;

struct DisasmLine {
    uint16_t addr = 0;       // address of the instruction
    int words = 1;           // instruction length in 16-bit words (1..3)
    std::string text;        // formatted "MNEM  src, dst"
};

// Disassemble one PDP-11/VM1 instruction at `addr` using side-effect-free reads.
// If `syms` is given, branch / jump / absolute / PC-relative target addresses are
// rendered as symbol names when one matches (falls back to octal).
DisasmLine disasm(const Memory& mem, uint16_t addr, const SymbolTable* syms = nullptr);

} // namespace bk
