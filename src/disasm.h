#pragma once
#include <QString>
#include <QByteArray>
#include <QVector>
#include <cstdint>

namespace rcx {

// ── One decoded x86 instruction ──
//
// The structured form of what `disassemble()` used to flatten into a single
// string. A blob is fine for a hover popup; a node type needs each instruction
// to carry its own offset and byte span, because every one becomes a row with
// its own address in the offset margin.
struct Instruction {
    int      offset = 0;      // bytes from the start of the buffer
    int      length = 0;      // bytes this instruction consumed (1..15)
    QString  text;            // "mov rax, [rcx]", or "db 0xcc" when !ok
    uint64_t target = 0;      // absolute branch/call target; 0 = not a branch
    bool     ok     = false;  // false = a raw byte we could not decode
};

// Decode a run of x86 code. `bitness` is 32 or 64; 16-bit is not supported by
// the decoder and returns nothing.
//
// Unlike the old loop, a byte that does not decode does NOT end the listing:
// it is emitted as a one-byte `db` and the sweep resynchronises on the next
// byte. Real code is full of data — jump tables, alignment padding, string
// literals — and a disassembler that gives up at the first of them shows you
// the first two instructions of a function and calls it a day.
//
// The returned lengths always sum to exactly min(bytes.size(), maxBytes), so a
// caller rendering a fixed byte span can rely on the span being filled.
QVector<Instruction> decodeRange(const QByteArray& bytes, uint64_t baseAddr,
                                 int bitness, int maxBytes = 128);

// Disassemble up to maxBytes of x86 code, returning formatted asm lines.
// bitness: 32 or 64. Returns one line per instruction, prefixed with offset.
QString disassemble(const QByteArray& bytes, uint64_t baseAddr, int bitness, int maxBytes = 128);

// Format bytes as hex dump lines (16 bytes per line with ASCII sidebar).
QString hexDump(const QByteArray& bytes, uint64_t baseAddr, int maxBytes = 128);

} // namespace rcx
