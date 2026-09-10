#include "disasm.h"

extern "C" {
#include <fadec.h>
}

namespace rcx {

QVector<Instruction> decodeRange(const QByteArray& bytes, uint64_t baseAddr,
                                 int bitness, int maxBytes) {
    QVector<Instruction> out;
    if (bytes.isEmpty() || (bitness != 32 && bitness != 64))
        return out;

    const int len = qMin((int)bytes.size(), maxBytes);
    const auto* buf = reinterpret_cast<const uint8_t*>(bytes.constData());

    int off = 0;
    while (off < len) {
        FdInstr instr;
        // Decode at address 0, deliberately. fadec's `address` parameter is
        // marked deprecated (fadec.h:140) and it is not merely a style
        // preference: decode.c:729 folds the address into the immediate and
        // leaves the operand typed FD_OT_IMM, which makes a branch target
        // indistinguishable from `mov eax, 5`. Passing 0 keeps the operand
        // typed FD_OT_OFF, so `target` below is recoverable.
        const int ret = fd_decode(buf + off, len - off, bitness, 0, &instr);

        if (ret < 0) {
            // A byte that is not the start of an instruction. Emit it raw and
            // step ONE byte, so the sweep resynchronises on the next boundary
            // instead of abandoning the rest of the range. FD_ERR_PARTIAL (the
            // buffer ending mid-instruction) lands here too and is handled the
            // same way: the remaining bytes come out as `db`, which is exactly
            // right for a fixed byte span whose tail is a cut-off instruction.
            Instruction bad;
            bad.offset = off;
            bad.length = 1;
            bad.ok     = false;
            bad.text   = QStringLiteral("db 0x%1")
                             .arg(buf[off], 2, 16, QLatin1Char('0'));
            out.push_back(bad);
            off += 1;
            continue;
        }

        // ...and the absolute address goes in at FORMAT time instead, which
        // is what fd_format_abs is for. This produces byte-identical text:
        // fd_format is literally fd_format_abs(instr, 0, ...) (format.c:540),
        // and format.c:506 adds `addr + FD_SIZE` back onto an FD_OT_OFF
        // operand — the same absolute value the old path baked in earlier.
        char fmtBuf[128];
        fd_format_abs(&instr, baseAddr + off, fmtBuf, sizeof(fmtBuf));

        Instruction ins;
        ins.offset = off;
        ins.length = ret;
        ins.ok     = true;
        ins.text   = QString::fromLatin1(fmtBuf);
        // A relative branch or a RIP-relative reference stores its
        // displacement as an FD_OT_OFF operand, measured from the END of the
        // instruction — the same arithmetic the formatter does.
        for (int i = 0; i < 4; i++) {
            if (FD_OP_TYPE(&instr, i) == FD_OT_OFF) {
                ins.target = baseAddr + off + (uint64_t)ret
                           + (uint64_t)FD_OP_IMM(&instr, i);
                break;
            }
        }
        out.push_back(ins);
        off += ret;
    }
    return out;
}

QString disassemble(const QByteArray& bytes, uint64_t baseAddr, int bitness, int maxBytes) {
    QString result;
    // Stops at the first undecodable byte rather than resyncing: this is the
    // hover popup's listing, where a truncated view is better than a column of
    // `db` bytes, and its output is pinned byte-for-byte by tests/test_disasm.
    for (const Instruction& ins : decodeRange(bytes, baseAddr, bitness, maxBytes)) {
        if (!ins.ok) break;
        if (!result.isEmpty())
            result += QLatin1Char('\n');
        result += QStringLiteral("%1  %2")
            .arg(baseAddr + ins.offset, bitness == 64 ? 16 : 8, 16, QLatin1Char('0'))
            .arg(ins.text);
    }
    return result;
}

QString hexDump(const QByteArray& bytes, uint64_t baseAddr, int maxBytes) {
    if (bytes.isEmpty())
        return {};

    int len = qMin((int)bytes.size(), maxBytes);
    QString result;

    for (int off = 0; off < len; off += 16) {
        int lineLen = qMin(16, len - off);

        if (!result.isEmpty())
            result += QLatin1Char('\n');

        // Address
        bool wide = (baseAddr + len > 0xFFFFFFFFULL);
        result += QStringLiteral("%1  ").arg(baseAddr + off, wide ? 16 : 8, 16, QLatin1Char('0'));

        // Hex bytes
        for (int i = 0; i < 16; i++) {
            if (i < lineLen) {
                uint8_t b = static_cast<uint8_t>(bytes[off + i]);
                result += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
            } else {
                result += QStringLiteral("   ");
            }
            if (i == 7) result += QLatin1Char(' ');
        }

        // ASCII
        result += QLatin1Char(' ');
        for (int i = 0; i < lineLen; i++) {
            char c = bytes[off + i];
            result += (c >= 0x20 && c < 0x7f) ? QLatin1Char(c) : QLatin1Char('.');
        }
    }
    return result;
}

} // namespace rcx
