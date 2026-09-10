#include <QtTest/QTest>
#include "disasm.h"
#include "core.h"
#include "providers/buffer_provider.h"

using namespace rcx;

// Helper: extract mnemonic portion from disassembly output (after "addr  ")
static QString mnemonic(const QString& line) {
    int sep = line.indexOf("  ");
    return sep >= 0 ? line.mid(sep + 2) : line;
}

class TestDisasm : public QObject {
    Q_OBJECT
private slots:
    // ──────────────────────────────────────────────────
    //  disassemble() unit tests – exact mnemonic match
    // ──────────────────────────────────────────────────

    void testDisasm64_pushMov() {
        QByteArray code("\x55\x48\x89\xe5", 4);
        QString result = disassemble(code, 0x401000, 64);
        QStringList lines = result.split('\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].startsWith("0000000000401000"));
        QVERIFY(lines[1].startsWith("0000000000401001"));
        QCOMPARE(mnemonic(lines[0]), QStringLiteral("push rbp"));
        QCOMPARE(mnemonic(lines[1]), QStringLiteral("mov rbp, rsp"));
    }

    void testDisasm64_ret()     { QCOMPARE(mnemonic(disassemble(QByteArray("\xc3",1), 0x7FF000, 64)), QStringLiteral("ret")); }
    void testDisasm64_nop()     { QCOMPARE(mnemonic(disassemble(QByteArray("\x90",1), 0, 64)), QStringLiteral("nop")); }
    void testDisasm64_xorEax()  { QCOMPARE(mnemonic(disassemble(QByteArray("\x31\xc0",2), 0, 64)), QStringLiteral("xor eax, eax")); }
    void testDisasm64_subRsp()  { QCOMPARE(mnemonic(disassemble(QByteArray("\x48\x83\xec\x20",4), 0, 64)), QStringLiteral("sub rsp, 0x20")); }
    void testDisasm64_int3()    { QCOMPARE(mnemonic(disassemble(QByteArray("\xcc",1), 0, 64)), QStringLiteral("int3")); }
    void testDisasm64_pushRdi() { QCOMPARE(mnemonic(disassemble(QByteArray("\x57",1), 0, 64)), QStringLiteral("push rdi")); }
    void testDisasm64_popRsi()  { QCOMPARE(mnemonic(disassemble(QByteArray("\x5e",1), 0, 64)), QStringLiteral("pop rsi")); }
    void testDisasm64_testEax() { QCOMPARE(mnemonic(disassemble(QByteArray("\x85\xc0",2), 0, 64)), QStringLiteral("test eax, eax")); }

    void testDisasm64_leaRipRel() {
        QCOMPARE(mnemonic(disassemble(QByteArray("\x48\x8d\x05\x10\x00\x00\x00",7), 0x1000, 64)),
                 QStringLiteral("lea rax, [rip+0x10]"));
    }
    void testDisasm64_callRel() {
        // call target = 0x1000 + 5 + 0x100 = 0x1105
        QCOMPARE(mnemonic(disassemble(QByteArray("\xe8\x00\x01\x00\x00",5), 0x1000, 64)),
                 QStringLiteral("call 0x1105"));
    }
    void testDisasm64_jmpRel() {
        // jmp target = 0x1000 + 2 + 0x10 = 0x1012
        QCOMPARE(mnemonic(disassemble(QByteArray("\xeb\x10",2), 0x1000, 64)),
                 QStringLiteral("jmp 0x1012"));
    }
    void testDisasm64_movMemRead() {
        QCOMPARE(mnemonic(disassemble(QByteArray("\x48\x8b\x43\x10",4), 0, 64)),
                 QStringLiteral("mov rax, qword ptr [rbx+0x10]"));
    }
    void testDisasm64_movMemWrite() {
        QCOMPARE(mnemonic(disassemble(QByteArray("\x48\x89\x4c\x24\x08",5), 0, 64)),
                 QStringLiteral("mov qword ptr [rsp+0x8], rcx"));
    }

    void testDisasm64_functionPrologue() {
        QByteArray code("\x55\x48\x89\xe5\x48\x83\xec\x20\xc3", 9);
        QStringList lines = disassemble(code, 0x140001000ULL, 64).split('\n');
        QCOMPARE(lines.size(), 4);
        QVERIFY(lines[0].startsWith("0000000140001000"));
        QCOMPARE(mnemonic(lines[0]), QStringLiteral("push rbp"));
        QCOMPARE(mnemonic(lines[1]), QStringLiteral("mov rbp, rsp"));
        QCOMPARE(mnemonic(lines[2]), QStringLiteral("sub rsp, 0x20"));
        QCOMPARE(mnemonic(lines[3]), QStringLiteral("ret"));
    }

    void testDisasm64_multipleNops() {
        QStringList lines = disassemble(QByteArray(5,'\x90'), 0x1000, 64).split('\n');
        QCOMPARE(lines.size(), 5);
        for (int i = 0; i < 5; i++) {
            QCOMPARE(mnemonic(lines[i]), QStringLiteral("nop"));
            QVERIFY(lines[i].startsWith(QStringLiteral("%1").arg(0x1000+i, 16, 16, QLatin1Char('0'))));
        }
    }

    void testDisasm32_pushMov() {
        QByteArray code("\x55\x89\xe5", 3);
        QStringList lines = disassemble(code, 0x401000, 32).split('\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[0].startsWith("00401000"));
        QCOMPARE(mnemonic(lines[0]), QStringLiteral("push ebp"));
        QCOMPARE(mnemonic(lines[1]), QStringLiteral("mov ebp, esp"));
    }

    void testDisasm_empty()          { QVERIFY(disassemble({}, 0, 64).isEmpty()); QVERIFY(disassemble({}, 0, 32).isEmpty()); }
    void testDisasm_invalidBitness() { QVERIFY(disassemble(QByteArray("\x90",1), 0, 16).isEmpty()); }
    void testDisasm_maxBytes()       { QCOMPARE(disassemble(QByteArray(200,'\x90'), 0, 64, 128).count('\n') + 1, 128); }
    void testDisasm64_addrWidth()    { QCOMPARE(disassemble(QByteArray("\x90",1), 0, 64).indexOf("  "), 16); }
    void testDisasm32_addrWidth()    { QCOMPARE(disassemble(QByteArray("\x90",1), 0, 32).indexOf("  "), 8); }


    // ──────────────────────────────────────────────────
    //  decodeRange() — the structured form the asm node renders
    // ──────────────────────────────────────────────────

    // Every instruction carries its own offset and byte span, because each one
    // becomes a row with its own address in the offset margin.
    void testDecodeRange_offsetsAndLengths() {
        // push rbp (1) ; mov rbp,rsp (3) ; nop (1)
        QByteArray code("\x55\x48\x89\xe5\x90", 5);
        const auto ins = decodeRange(code, 0x401000, 64);
        QCOMPARE(ins.size(), 3);
        QCOMPARE(ins[0].offset, 0);  QCOMPARE(ins[0].length, 1);
        QCOMPARE(ins[1].offset, 1);  QCOMPARE(ins[1].length, 3);
        QCOMPARE(ins[2].offset, 4);  QCOMPARE(ins[2].length, 1);
        QCOMPARE(ins[0].text, QStringLiteral("push rbp"));
        QCOMPARE(ins[1].text, QStringLiteral("mov rbp, rsp"));
        QCOMPARE(ins[2].text, QStringLiteral("nop"));
        for (const auto& i : ins) QVERIFY(i.ok);
    }

    // THE property a fixed byte span depends on: the decoded lengths add up to
    // exactly the span, so a node declaring 16 bytes always renders 16 bytes.
    void testDecodeRange_lengthsFillTheSpanExactly() {
        const QList<QByteArray> cases = {
            QByteArray("\x55\x48\x89\xe5\x90", 5),           // clean
            QByteArray("\xcc\xcc\xcc", 3),                    // all int3
            QByteArray("\x0f\x0b\x90\xff\xff\xff", 6),        // ud2 then junk
            QByteArray("\x48\x8b", 2),                        // truncated mov
        };
        for (const QByteArray& code : cases) {
            int sum = 0;
            for (const auto& i : decodeRange(code, 0x1000, 64)) sum += i.length;
            QCOMPARE(sum, code.size());
        }
        // ...and honours maxBytes as the span, not the buffer.
        int sum = 0;
        for (const auto& i : decodeRange(QByteArray(200, '\x90'), 0, 64, 16)) sum += i.length;
        QCOMPARE(sum, 16);
    }

    // The behaviour the old flat wrapper could not have: a byte that does not
    // decode is one `db` row and the sweep CONTINUES. Real code is full of
    // data — jump tables, padding, literals — and stopping at the first of
    // them showed two instructions and gave up.
    void testDecodeRange_resyncsAfterAnUndecodableByte() {
        // 0xff 0xff is not a valid instruction; a nop follows it.
        QByteArray code("\xff\xff\x90\x90", 4);
        const auto ins = decodeRange(code, 0x2000, 64);
        QVERIFY2(ins.size() >= 2, "the sweep stopped at the bad byte");
        QVERIFY(!ins.first().ok);
        QCOMPARE(ins.first().length, 1);
        QCOMPARE(ins.first().text, QStringLiteral("db 0xff"));
        // A real instruction is reached after the junk.
        bool foundNop = false;
        for (const auto& i : ins) if (i.ok && i.text == QStringLiteral("nop")) foundNop = true;
        QVERIFY2(foundNop, "never resynchronised onto the nop");
        // And the old flat listing still stops there, unchanged.
        QVERIFY(disassemble(code, 0x2000, 64).isEmpty());
    }

    // A span whose last instruction is cut off: the tail comes out as raw
    // bytes rather than being silently dropped, so the span stays filled.
    void testDecodeRange_truncatedTailBecomesRawBytes() {
        QByteArray code("\x90\x48\x8b", 3);   // nop, then a truncated mov
        const auto ins = decodeRange(code, 0x3000, 64);
        QVERIFY(ins.size() >= 2);
        QVERIFY(ins[0].ok);
        QCOMPARE(ins[0].text, QStringLiteral("nop"));
        QVERIFY2(!ins[1].ok, "the cut-off instruction was decoded anyway");
        int sum = 0;
        for (const auto& i : ins) sum += i.length;
        QCOMPARE(sum, 3);
    }

    // Branch targets are resolved absolutely, which is what makes a `call`
    // row worth clicking.
    void testDecodeRange_branchTargetIsAbsolute() {
        // e8 rel32 = call. rel32 = 0x00000005 -> target = next insn + 5.
        QByteArray code("\xe8\x05\x00\x00\x00", 5);
        const auto ins = decodeRange(code, 0x401000, 64);
        QCOMPARE(ins.size(), 1);
        QVERIFY(ins[0].ok);
        QCOMPARE(ins[0].target, (uint64_t)(0x401000 + 5 + 5));
        // A non-branch reports no target at all.
        const auto nop = decodeRange(QByteArray("\x90", 1), 0x401000, 64);
        QCOMPARE(nop.size(), 1);
        QCOMPARE(nop[0].target, (uint64_t)0);
    }

    void testDecodeRange_emptyAndBadBitness() {
        QVERIFY(decodeRange({}, 0, 64).isEmpty());
        QVERIFY(decodeRange(QByteArray("\x90", 1), 0, 16).isEmpty());
    }

    // ──────────────────────────────────────────────────
    //  hexDump() unit tests
    // ──────────────────────────────────────────────────

    void testHexDump_basic() {
        QByteArray data; for (int i=0;i<32;i++) data.append((char)i);
        QString r = hexDump(data, 0x1000, 128);
        QCOMPARE(r.count('\n')+1, 2);
        QVERIFY(r.startsWith("00001000"));
    }
    void testHexDump_ascii() {
        QVERIFY(hexDump(QByteArray("Hello, World!xx",15), 0, 128).contains("Hello"));
    }
    void testHexDump_nonPrintable() {
        QByteArray d(16,'\0'); d[0]='A'; d[15]='Z';
        QVERIFY(hexDump(d, 0, 128).contains("A..............Z"));
    }
    void testHexDump_empty()     { QVERIFY(hexDump({}, 0).isEmpty()); }
    void testHexDump_maxBytes()  { QCOMPARE(hexDump(QByteArray(200,'\xAA'), 0, 64).count('\n')+1, 4); }
    void testHexDump_wideAddr()  { QVERIFY(hexDump(QByteArray(16,'\0'), 0x100000000ULL, 128).startsWith("0000000100000000")); }
    void testHexDump_hexValues() {
        QByteArray d; d.append('\xDE'); d.append('\xAD'); d.append('\xBE'); d.append('\xEF');
        while (d.size()<16) d.append('\0');
        QVERIFY(hexDump(d, 0, 128).contains("de ad be ef", Qt::CaseInsensitive));
    }
    void testHexDump_secondLineAddr() {
        QStringList lines = hexDump(QByteArray(32,'\x42'), 0x2000, 128).split('\n');
        QCOMPARE(lines.size(), 2);
        QVERIFY(lines[1].startsWith("00002010"));
    }


    // ──────────────────────────────────────────────────
    //  The Asm node kind — instruction rows, in place
    // ──────────────────────────────────────────────────

    // Builds: struct { hex64 before; asm[N] code; hex64 after; }
    // and returns the composed lines. `code` is written at offset 8.
    static ComposeResult composeAsmStruct(QByteArray code, int span,
                                          BufferProvider** provOut, NodeTree* treeOut) {
        static QByteArray mem;
        mem = QByteArray(256, '\0');
        memcpy(mem.data() + 8, code.constData(), qMin(code.size(), (qsizetype)span));
        static BufferProvider prov(mem);
        prov = BufferProvider(mem);
        if (provOut) *provOut = &prov;

        NodeTree tree;
        tree.baseAddress = 0;
        Node root; root.kind = NodeKind::Struct; root.name = "Obj";
        root.parentId = 0; root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node before; before.kind = NodeKind::Hex64; before.name = "before";
        before.parentId = rootId; before.offset = 0;
        tree.addNode(before);

        Node asmNode; asmNode.kind = NodeKind::Asm; asmNode.name = "code";
        asmNode.parentId = rootId; asmNode.offset = 8; asmNode.arrayLen = span;
        // Node::collapsed defaults to TRUE (it exists for containers). The
        // controller clears it when it creates an asm node; do the same here.
        asmNode.collapsed = false;
        tree.addNode(asmNode);

        Node after; after.kind = NodeKind::Hex64; after.name = "after";
        after.parentId = rootId; after.offset = 8 + span;
        tree.addNode(after);

        if (treeOut) *treeOut = tree;
        return compose(tree, prov);
    }

    // THE invariant a fixed byte window exists to protect: whatever the code
    // in the span decodes to, the field after it does not move.
    void testAsmNode_spanIsFixedSoSiblingsNeverMove() {
        // Three very different codes in the same 16-byte window.
        const QList<QByteArray> codes = {
            QByteArray("\x55\x48\x89\xe5\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90\x90", 16),
            QByteArray("\xe9\x00\x00\x00\x00\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc\xcc", 16),
            QByteArray("\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff", 16),
        };
        for (const QByteArray& code : codes) {
            NodeTree tree;
            ComposeResult r = composeAsmStruct(code, 16, nullptr, &tree);
            // Find the "after" row and check its address.
            bool found = false;
            for (const LineMeta& lm : r.meta) {
                if (lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) continue;
                if (tree.nodes[lm.nodeIdx].name != QStringLiteral("after")) continue;
                QCOMPARE(lm.offsetAddr, (uint64_t)(8 + 16));
                found = true;
                break;
            }
            QVERIFY2(found, "the field after the asm node vanished");
        }
    }

    // One row per instruction, each carrying its own address, so a click lands
    // on the right instruction and the offset margin needs no special casing.
    void testAsmNode_oneRowPerInstructionWithItsOwnAddress() {
        // push rbp (1) ; mov rbp,rsp (3) ; ret (1) ; then padding
        QByteArray code("\x55\x48\x89\xe5\xc3", 5);
        code.append(QByteArray(11, '\x90'));   // nops fill the rest of the 16
        NodeTree tree;
        ComposeResult r = composeAsmStruct(code, 16, nullptr, &tree);

        QVector<uint64_t> addrs;
        QStringList texts;
        const QStringList lines = r.text.split('\n');
        for (int i = 0; i < r.meta.size(); i++) {
            const LineMeta& lm = r.meta[i];
            if (lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) continue;
            if (tree.nodes[lm.nodeIdx].kind != NodeKind::Asm) continue;
            if (!lm.isMemberLine) continue;
            addrs << lm.offsetAddr;
            if (i < lines.size()) texts << lines[i];
        }
        QVERIFY2(addrs.size() >= 3, qPrintable(QStringLiteral("only %1 instruction rows").arg(addrs.size())));
        // Addresses are the node's, plus each instruction's own offset.
        QCOMPARE(addrs[0], (uint64_t)8);
        QCOMPARE(addrs[1], (uint64_t)9);
        QCOMPARE(addrs[2], (uint64_t)12);
        // Strictly increasing, and every one inside the declared span.
        for (int i = 1; i < addrs.size(); i++) QVERIFY(addrs[i] > addrs[i-1]);
        QVERIFY(addrs.last() < 8 + 16);
        // The mnemonics really are there, with their raw bytes beside them.
        QVERIFY2(texts[0].contains(QStringLiteral("push rbp")), qPrintable(texts[0]));
        QVERIFY2(texts[0].contains(QStringLiteral("55")), qPrintable(texts[0]));
        QVERIFY2(texts[1].contains(QStringLiteral("mov rbp, rsp")), qPrintable(texts[1]));
        QVERIFY2(texts[2].contains(QStringLiteral("ret")), qPrintable(texts[2]));
    }

    // Instruction rows are MEMBER lines, which is what makes them read-only —
    // typeSpanFor / nameSpanFor / valueSpanFor all refuse a member line, so no
    // inline edit can start on one. This is the whole reason for modelling the
    // emission on enum members rather than on Mat4x4's continuations.
    void testAsmNode_instructionRowsAreNotEditable() {
        QByteArray code("\x55\x48\x89\xe5", 4);
        code.append(QByteArray(12, '\x90'));
        NodeTree tree;
        ComposeResult r = composeAsmStruct(code, 16, nullptr, &tree);
        int checked = 0;
        for (const LineMeta& lm : r.meta) {
            if (lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) continue;
            if (tree.nodes[lm.nodeIdx].kind != NodeKind::Asm || !lm.isMemberLine) continue;
            QVERIFY2(!typeSpanFor(lm).valid,  "an instruction row offered a type edit");
            QVERIFY2(!nameSpanFor(lm).valid,  "an instruction row offered a name edit");
            QVERIFY2(!valueSpanFor(lm, 200).valid, "an instruction row offered a value edit");
            checked++;
        }
        QVERIFY(checked > 0);
    }

    // Collapsed: the header only. The node keeps its footprint either way.
    void testAsmNode_collapsedHidesTheInstructions() {
        QByteArray code("\x55\x48\x89\xe5", 4);
        code.append(QByteArray(12, '\x90'));
        NodeTree tree;
        ComposeResult open = composeAsmStruct(code, 16, nullptr, &tree);
        int openRows = 0;
        for (const LineMeta& lm : open.meta)
            if (lm.nodeIdx >= 0 && lm.nodeIdx < tree.nodes.size()
                && tree.nodes[lm.nodeIdx].kind == NodeKind::Asm && lm.isMemberLine) openRows++;
        QVERIFY(openRows > 0);

        // Same tree, node collapsed.
        for (Node& n : tree.nodes) if (n.kind == NodeKind::Asm) n.collapsed = true;
        QByteArray mem(256, '\0');
        memcpy(mem.data() + 8, code.constData(), 16);
        BufferProvider prov(mem);
        ComposeResult shut = compose(tree, prov);
        int shutRows = 0;
        for (const LineMeta& lm : shut.meta)
            if (lm.nodeIdx >= 0 && lm.nodeIdx < tree.nodes.size()
                && tree.nodes[lm.nodeIdx].kind == NodeKind::Asm && lm.isMemberLine) shutRows++;
        QCOMPARE(shutRows, 0);
    }

    // The type column carries the span, because the span is the point of the
    // node and KindMeta::typeName cannot hold per-node data.
    void testAsmNode_typeColumnShowsTheSpan() {
        QCOMPARE(fmt::asmTypeName(16), QStringLiteral("asm[16]"));
        QCOMPARE(fmt::asmTypeName(0),  QStringLiteral("asm[0]"));
        Node n; n.kind = NodeKind::Asm; n.arrayLen = 24;
        QCOMPARE(n.byteSize(), 24);
        QByteArray code(24, '\x90');
        NodeTree tree;
        ComposeResult r = composeAsmStruct(code, 24, nullptr, &tree);
        QVERIFY2(r.text.contains(QStringLiteral("asm[24]")), qPrintable(r.text.left(400)));
    }


    // The window is the only thing that must survive a save: everything else
    // about the node is recomputed from the target's bytes on load.
    void testAsmNode_roundTripsThroughJson() {
        Node n;
        n.kind      = NodeKind::Asm;
        n.name      = QStringLiteral("hook");
        n.offset    = 8;
        n.arrayLen  = 24;
        n.collapsed = false;
        n.comment   = QStringLiteral("inline patch");

        Node back = Node::fromJson(n.toJson());
        QCOMPARE(back.kind, NodeKind::Asm);
        QCOMPARE(back.name, QStringLiteral("hook"));
        QCOMPARE(back.offset, 8);
        QCOMPARE(back.arrayLen, 24);
        QCOMPARE(back.comment, QStringLiteral("inline patch"));
        // Collapsed state deliberately does NOT survive: core.h's fromJson
        // loads every node collapsed ("user expands as needed") so a big
        // document cannot explode on open. An asm node is no exception — its
        // span is user-set and could be thousands of rows.
        QCOMPARE(back.collapsed, true);
        QCOMPARE(back.byteSize(), 24);
        // Name-keyed, so the string in the file is the contract.
        QCOMPARE(n.toJson()["kind"].toString(), QStringLiteral("Asm"));
        QCOMPARE(kindFromString(QStringLiteral("Asm")), NodeKind::Asm);
    }

    // The footprint is arrayLen and nothing else — not the table's size, not
    // the number of instructions that happen to fit.
    void testAsmNode_footprintIsTheDeclaredWindow() {
        Node n; n.kind = NodeKind::Asm;
        for (int span : {0, 1, 15, 16, 4096}) {
            n.arrayLen = span;
            QCOMPARE(n.byteSize(), span);
        }
        // The table entry stays 1: the real size lives on the node, exactly
        // as it does for UTF8/strLen.
        QCOMPARE(sizeForKind(NodeKind::Asm), 1);
        QCOMPARE(linesForKind(NodeKind::Asm), 1);
        QVERIFY(isCodeKind(NodeKind::Asm));
        QVERIFY(!isCodeKind(NodeKind::Hex8));
        // And it is NOT a container, so structSpan's short-circuit gives the
        // window rather than walking children it does not have.
        QVERIFY(!isContainerKind(NodeKind::Asm));
    }

    // ──────────────────────────────────────────────────
    //  End-to-end: pointer-expanded VTable with FuncPtr64
    //  Verifies we read from the COMPOSED address, not node.offset
    // ──────────────────────────────────────────────────

    void testVTableDisasm_composedAddress() {
        // Memory layout (absolute addresses, baseAddress = 0):
        //
        //   [0x0000]  Root "Obj" struct
        //     +0x00: Pointer64 __vptr => points to 0x100 (vtable)
        //
        //   [0x0100]  VTable (expanded via pointer deref)
        //     +0x00: func ptr 0 => value 0x200 (func0 code)
        //     +0x08: func ptr 1 => value 0x300 (func1 code)
        //
        //   [0x0200]  func0 code: push rbp; ret
        //   [0x0300]  func1 code: xor eax, eax; ret
        //

        // Build a 4KB buffer
        QByteArray mem(4096, '\0');
        auto w64 = [&](int off, uint64_t val) {
            memcpy(mem.data() + off, &val, 8);
        };

        // Root object at offset 0: __vptr points to vtable at 0x100
        w64(0x00, 0x100);

        // VTable at offset 0x100: two function pointers
        w64(0x100, 0x200);  // slot 0 -> func0
        w64(0x108, 0x300);  // slot 1 -> func1

        // func0 at offset 0x200: push rbp; ret
        mem[0x200] = '\x55';
        mem[0x201] = '\xc3';

        // func1 at offset 0x300: xor eax, eax; ret
        mem[0x300] = '\x31';
        mem[0x301] = '\xc0';
        mem[0x302] = '\xc3';

        BufferProvider prov(mem);

        // Build node tree
        NodeTree tree;
        tree.baseAddress = 0;

        // Root struct "Obj"
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Obj";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        // VTable struct definition (template)
        Node vtDef;
        vtDef.kind = NodeKind::Struct;
        vtDef.name = "VTable";
        vtDef.parentId = 0;
        vtDef.offset = 0x1000; // parked far away so it doesn't overlap
        int vti = tree.addNode(vtDef);
        uint64_t vtId = tree.nodes[vti].id;

        // Two FuncPtr64 children inside VTable definition
        Node fp0;
        fp0.kind = NodeKind::FuncPtr64;
        fp0.name = "func0";
        fp0.parentId = vtId;
        fp0.offset = 0;
        tree.addNode(fp0);

        Node fp1;
        fp1.kind = NodeKind::FuncPtr64;
        fp1.name = "func1";
        fp1.parentId = vtId;
        fp1.offset = 8;
        tree.addNode(fp1);

        // Pointer64 "__vptr" in root, pointing to VTable via refId
        Node vptr;
        vptr.kind = NodeKind::Pointer64;
        vptr.name = "__vptr";
        vptr.parentId = rootId;
        vptr.offset = 0;
        vptr.refId = vtId;
        vptr.collapsed = false;
        tree.addNode(vptr);

        // Compose the tree
        ComposeResult result = compose(tree, prov);

        // Find the FuncPtr64 lines in the composed output that are inside the
        // pointer-expanded VTable (near vtable address), not the standalone definition.
        struct FuncInfo { int line; uint64_t offsetAddr; NodeKind kind; QString name; };
        QVector<FuncInfo> funcPtrs;
        for (int i = 0; i < result.meta.size(); i++) {
            const LineMeta& lm = result.meta[i];
            if (lm.nodeKind == NodeKind::FuncPtr64 && lm.lineKind == LineKind::Field) {
                // Only include the pointer-expanded ones (near vtable at 0x100)
                if (lm.offsetAddr >= 0x100 && lm.offsetAddr < 0x200) {
                    int nodeIdx = lm.nodeIdx;
                    funcPtrs.push_back(FuncInfo{i, lm.offsetAddr, lm.nodeKind,
                                     nodeIdx >= 0 ? tree.nodes[nodeIdx].name : QString()});
                }
            }
        }

        QCOMPARE(funcPtrs.size(), 2);

        // Verify composed addresses point to the vtable, NOT to the root struct
        // func0 should be at 0x100 (vtable + 0)
        QCOMPARE(funcPtrs[0].offsetAddr, (uint64_t)0x100);
        // func1 should be at 0x108 (vtable + 8)
        QCOMPARE(funcPtrs[1].offsetAddr, (uint64_t)0x108);

        // Now simulate what the hover code should do:
        // Read the function pointer VALUE from the correct provider address
        for (const auto& fp : funcPtrs) {
            // Provider reads at absolute address directly
            uint64_t provAddr = fp.offsetAddr;

            // Read the pointer value (the function address)
            uint64_t ptrVal = prov.readU64(provAddr);

            // Verify we got the right pointer values
            if (fp.name == "func0") {
                QCOMPARE(ptrVal, (uint64_t)0x200);
            } else {
                QCOMPARE(ptrVal, (uint64_t)0x300);
            }

            // Read code bytes at the pointer target (absolute address)
            uint64_t codeProvAddr = ptrVal;
            QByteArray codeBytes = prov.readBytes(codeProvAddr, 128);

            // Disassemble and verify
            QString asm_ = disassemble(codeBytes, ptrVal, 64, 128);
            QVERIFY2(!asm_.isEmpty(), qPrintable("Empty disasm for " + fp.name));

            QStringList lines = asm_.split('\n');
            if (fp.name == "func0") {
                // Should decode: push rbp; ret
                QVERIFY2(lines.size() >= 2, qPrintable(QString("Expected >= 2 lines for func0, got %1: %2").arg(lines.size()).arg(asm_)));
                QCOMPARE(mnemonic(lines[0]), QStringLiteral("push rbp"));
                QCOMPARE(mnemonic(lines[1]), QStringLiteral("ret"));
                // Verify address in output matches the real function address
                QVERIFY2(lines[0].contains("200"),
                         qPrintable("func0 addr wrong: " + lines[0]));
            } else {
                // Should decode: xor eax, eax; ret
                QVERIFY2(lines.size() >= 2, qPrintable(QString("Expected >= 2 lines for func1, got %1: %2").arg(lines.size()).arg(asm_)));
                QCOMPARE(mnemonic(lines[0]), QStringLiteral("xor eax, eax"));
                QCOMPARE(mnemonic(lines[1]), QStringLiteral("ret"));
                QVERIFY2(lines[0].contains("300"),
                         qPrintable("func1 addr wrong: " + lines[0]));
            }
        }

        // CRITICAL: Verify that reading from node.offset (the WRONG way) gives
        // different/wrong results. node.offset for func0=0, func1=8, which are
        // inside the ROOT struct, not the vtable.
        uint64_t wrongVal0 = prov.readU64(0);  // node.offset=0: reads __vptr value
        uint64_t wrongVal1 = prov.readU64(8);  // node.offset=8: reads garbage after __vptr
        // wrongVal0 = 0x100 (the vptr itself, NOT a function address)
        QCOMPARE(wrongVal0, (uint64_t)0x100);
        // This is the vtable address, not a function — disassembling it would be wrong
        QVERIFY2(wrongVal0 != (uint64_t)0x200,
                 "node.offset reads the vptr, not the function pointer");
        QVERIFY2(wrongVal1 != (uint64_t)0x300,
                 "node.offset=8 reads past vptr, not the second function pointer");
    }

    void testVTableDisasm_wrongAddressGivesWrongCode() {
        // Demonstrate that using node.offset instead of composed address
        // gives completely wrong disassembly results
        QByteArray mem(1024, '\0');
        auto w64 = [&](int off, uint64_t val) { memcpy(mem.data()+off, &val, 8); };

        // Root at 0: vptr -> 0x80
        w64(0x00, (uint64_t)0x80);
        // VTable at 0x80: one func ptr -> 0x100
        w64(0x80, (uint64_t)0x100);
        // Code at 0x100: sub rsp, 0x28; nop; ret
        mem[0x100] = '\x48'; mem[0x101] = '\x83'; mem[0x102] = '\xec';
        mem[0x103] = '\x28'; mem[0x104] = '\x90'; mem[0x105] = '\xc3';

        BufferProvider prov(mem);

        // WRONG: read from node.offset=0 (root's vptr value, not the func ptr)
        uint64_t wrongPtrVal = prov.readU64(0);
        QCOMPARE(wrongPtrVal, (uint64_t)0x80);  // This is the vtable addr, not a function!

        // RIGHT: read from composed address (vtable + 0)
        uint64_t rightPtrVal = prov.readU64(0x80);
        QCOMPARE(rightPtrVal, (uint64_t)0x100);  // This IS the function address

        // Disassemble the RIGHT target
        QByteArray rightCode = prov.readBytes(0x100, 128);
        QString rightAsm = disassemble(rightCode, 0x100, 64, 128);
        QStringList rightLines = rightAsm.split('\n');
        QVERIFY(rightLines.size() >= 3);
        QCOMPARE(mnemonic(rightLines[0]), QStringLiteral("sub rsp, 0x28"));
        QCOMPARE(mnemonic(rightLines[1]), QStringLiteral("nop"));
        QCOMPARE(mnemonic(rightLines[2]), QStringLiteral("ret"));

        // Disassemble the WRONG target (vtable data, not code!)
        QByteArray wrongCode = prov.readBytes(0x80, 128);
        QString wrongAsm = disassemble(wrongCode, 0x80, 64, 128);
        // The wrong bytes are the vtable entries (pointer values),
        // which decode as garbage instructions, not sub/nop/ret
        QVERIFY2(!wrongAsm.contains("sub rsp"),
                 qPrintable("Wrong address should NOT produce sub rsp: " + wrongAsm));
    }

    void testHoverFlow_fullSimulation() {
        // Full simulation of the hover flow as implemented in editor.cpp:
        //
        // 1. Compose the tree to get LineMeta with correct offsetAddr
        // 2. For each FuncPtr64 line, read pointer value from provider
        //    using lm.offsetAddr (absolute address)
        // 3. Read code bytes from the REAL provider using ptrVal directly
        //    (the real provider can read any process address; snapshot cannot)
        // 4. Disassemble the code bytes
        //
        // The key distinction: step 2 reads from composed tree addresses (in
        // the snapshot), step 3 reads from arbitrary code addresses (needs
        // the real provider, not snapshot).

        QByteArray mem(8192, '\0');
        auto w64 = [&](int off, uint64_t val) {
            memcpy(mem.data() + off, &val, 8);
        };

        // Layout:
        // [0x000] Root struct: __vptr -> vtable at 0x100
        // [0x100] VTable: func0 -> 0x1000, func1 -> 0x1800
        // [0x1000] func0 code: push rbp; mov rbp, rsp; sub rsp, 0x20; ret
        // [0x1800] func1 code: xor eax, eax; ret
        w64(0x000, (uint64_t)0x100);                   // __vptr
        w64(0x100, (uint64_t)0x1000);                   // vtable[0]
        w64(0x108, (uint64_t)0x1800);                   // vtable[1]
        // func0 code
        memcpy(mem.data() + 0x1000, "\x55\x48\x89\xe5\x48\x83\xec\x20\xc3", 9);
        // func1 code
        memcpy(mem.data() + 0x1800, "\x31\xc0\xc3", 3);

        // This provider represents the real process memory.
        BufferProvider realProv(mem);

        // Build a snapshot that only contains tree-data pages (like the
        // async refresh does). The snapshot does NOT contain function code pages.
        // This simulates the real scenario where SnapshotProvider only has
        // pages for the root struct and pointer-expanded structs.
        QByteArray snapData(0x200, '\0');   // only pages for root + vtable
        memcpy(snapData.data(), mem.constData(), 0x200);
        BufferProvider snapProv(snapData);

        // Build node tree
        NodeTree tree;
        tree.baseAddress = 0;

        Node root; root.kind = NodeKind::Struct; root.name = "Obj";
        root.parentId = 0; root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node vtDef; vtDef.kind = NodeKind::Struct; vtDef.name = "VTable";
        vtDef.parentId = 0; vtDef.offset = 0x2000;
        int vti = tree.addNode(vtDef);
        uint64_t vtId = tree.nodes[vti].id;

        Node fp0; fp0.kind = NodeKind::FuncPtr64; fp0.name = "func0";
        fp0.parentId = vtId; fp0.offset = 0;
        tree.addNode(fp0);
        Node fp1; fp1.kind = NodeKind::FuncPtr64; fp1.name = "func1";
        fp1.parentId = vtId; fp1.offset = 8;
        tree.addNode(fp1);

        Node vptr; vptr.kind = NodeKind::Pointer64; vptr.name = "__vptr";
        vptr.parentId = rootId; vptr.offset = 0; vptr.refId = vtId;
        vptr.collapsed = false;
        tree.addNode(vptr);

        // Compose with the snapshot (like production: compose uses snapshot)
        ComposeResult result = compose(tree, snapProv);

        // Find expanded FuncPtr64 lines
        for (int i = 0; i < result.meta.size(); i++) {
            const LineMeta& lm = result.meta[i];
            if (lm.nodeKind != NodeKind::FuncPtr64 || lm.lineKind != LineKind::Field)
                continue;
            if (lm.offsetAddr < 0x100 || lm.offsetAddr >= 0x200)
                continue;  // skip standalone VTable definition entries

            // --- Hover step 1: read pointer value from snapshot ---
            uint64_t provAddr = lm.offsetAddr;
            // The snapshot has this data (vtable pages are in it)
            QVERIFY2(snapProv.isReadable(provAddr, 8),
                     qPrintable(QString("Snapshot should have vtable page at %1")
                                .arg(provAddr, 0, 16)));
            uint64_t ptrVal = snapProv.readU64(provAddr);
            QVERIFY2(ptrVal != 0, "Function pointer should not be zero");

            // --- Hover step 2: read code from REAL provider ---
            // The snapshot does NOT have the code pages:
            uint64_t codeAddr = ptrVal;
            QVERIFY2(!snapProv.isReadable(codeAddr, 1),
                     "Snapshot should NOT have function code pages");
            // But the real provider does:
            QByteArray codeBytes(128, Qt::Uninitialized);
            bool readOk = realProv.read(codeAddr, codeBytes.data(), 128);
            QVERIFY2(readOk, "Real provider should be able to read code bytes");

            // --- Hover step 3: disassemble ---
            QString asm_ = disassemble(codeBytes, ptrVal, 64, 128);
            QVERIFY2(!asm_.isEmpty(), qPrintable("Empty disasm for line " + QString::number(i)));

            QStringList lines = asm_.split('\n');
            const Node& node = tree.nodes[lm.nodeIdx];
            if (node.name == "func0") {
                QVERIFY(lines.size() >= 4);
                QCOMPARE(mnemonic(lines[0]), QStringLiteral("push rbp"));
                QCOMPARE(mnemonic(lines[1]), QStringLiteral("mov rbp, rsp"));
                QCOMPARE(mnemonic(lines[2]), QStringLiteral("sub rsp, 0x20"));
                QCOMPARE(mnemonic(lines[3]), QStringLiteral("ret"));
            } else if (node.name == "func1") {
                QVERIFY(lines.size() >= 2);
                QCOMPARE(mnemonic(lines[0]), QStringLiteral("xor eax, eax"));
                QCOMPARE(mnemonic(lines[1]), QStringLiteral("ret"));
            }
        }
    }
};

QTEST_MAIN(TestDisasm)
#include "test_disasm.moc"
