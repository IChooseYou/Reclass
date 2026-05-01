#include "rtti.h"
#include "providers/buffer_provider.h"
#include "providers/provider.h"
#include <QTest>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <memory>

using namespace rcx;

// Build a synthetic MSVC RTTI structure inside a flat buffer. The layout
// mirrors what the MSVC compiler emits in .rdata for x64. A BufferProvider
// then hands those bytes to walkRtti(), which has no idea the source is
// synthetic. This lets us exercise every branch of the parser on every
// platform — no Windows / no real binary needed.
//
// Memory layout (offsets relative to imageBase = 0x10000):
//
//   0x0000 .. 0x0FFF  reserved (so addr 0 is invalid)
//   0x1000  vtable (5 method pointers, code addresses inside [0x100..0x1000) the .text "module")
//   0x1100  TypeDescriptor for "Foo"          (.?AVFoo@@)
//   0x1200  TypeDescriptor for "Bar"          (.?AVBar@@)
//   0x1300  TypeDescriptor for "Baz"          (.?AVBaz@@)
//   0x1400  RTTIClassHierarchyDescriptor (3 bases)
//   0x1500  array of 3 RVAs → BCDs
//   0x1600  BCD for Foo
//   0x1700  BCD for Bar
//   0x1800  BCD for Baz
//   0x1900  RTTICompleteObjectLocator
//
// vtable[-1] (i.e. 0x0FF8 in image space) = COL VA = imageBase + 0x1900.

static constexpr uint64_t kImageBase = 0x10000;

class TestRtti : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        m_buf.resize(0x10000, '\0');
        layoutSyntheticRtti(m_buf, kImageBase);
        // The provider's "addr" is offset from start of buffer; we want
        // virtual addresses that match our layout — so allocate a buffer
        // that begins at VA 0 and place RTTI at VA 0x10000 + offsets.
        // BufferProvider treats addr as buffer offset, so we need a buffer
        // big enough that offset == VA. Bigger but simpler than translating.
        m_data.resize(kImageBase + m_buf.size(), '\0');
        std::memcpy(m_data.data() + kImageBase, m_buf.constData(), m_buf.size());
    }

    void demangleBasic() {
        QCOMPARE(demangleRttiName(QStringLiteral(".?AVFoo@@")),
                 QStringLiteral("Foo"));
        QCOMPARE(demangleRttiName(QStringLiteral(".?AUStruct@@")),
                 QStringLiteral("Struct"));
    }

    void demangleNested() {
        QCOMPARE(demangleRttiName(QStringLiteral(".?AVBar@Foo@@")),
                 QStringLiteral("Foo::Bar"));
        QCOMPARE(demangleRttiName(QStringLiteral(".?AVZ@Y@X@@")),
                 QStringLiteral("X::Y::Z"));
    }

    void demangleMalformed() {
        // Non-RTTI input passes through unchanged
        QCOMPARE(demangleRttiName(QStringLiteral("plain_name")),
                 QStringLiteral("plain_name"));
        QCOMPARE(demangleRttiName(QString()), QString());
    }

    void walkSyntheticRtti() {
        BufferProvider prov(m_data, QStringLiteral("synthetic"));

        uint64_t vtableVa = kImageBase + 0x1000;
        auto info = walkRtti(prov, vtableVa, /*ptrSize=*/8, /*maxSlots=*/16);

        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(info.vtableAddress, vtableVa);
        QCOMPARE(info.completeLocator, kImageBase + 0x1900);
        QCOMPARE(info.imageBase, kImageBase);
        QCOMPARE(info.offset, 0);

        // Top class: Foo
        QCOMPARE(info.rawName, QStringLiteral(".?AVFoo@@"));
        QCOMPARE(info.demangledName, QStringLiteral("Foo"));

        // Three bases (we wrote 3 BCDs into the array)
        QCOMPARE(info.bases.size(), 3);
        QCOMPARE(info.bases[0].demangledName, QStringLiteral("Foo"));
        QCOMPARE(info.bases[1].demangledName, QStringLiteral("Bar"));
        QCOMPARE(info.bases[2].demangledName, QStringLiteral("Baz"));

        // Vtable: 5 valid slots (we filled 5 method pointers) — slot 6 is null.
        QCOMPARE(info.vtable.size(), 5);
        for (int i = 0; i < 5; i++) {
            QCOMPARE(info.vtable[i].slot, i);
            QCOMPARE(info.vtable[i].address, kImageBase + 0x100 + (uint64_t)i * 0x10);
        }
    }

    void walkRejectsBadSignature() {
        // Corrupt the COL signature — must be 0 or 1.
        QByteArray bad = m_data;
        // COL is at imageBase + 0x1900; signature is the first u32.
        uint32_t corrupt = 0xDEAD;
        std::memcpy(bad.data() + kImageBase + 0x1900, &corrupt, 4);

        BufferProvider prov(bad, QStringLiteral("bad"));
        auto info = walkRtti(prov, kImageBase + 0x1000);
        QVERIFY(!info.ok);
        QVERIFY(info.error.contains(QStringLiteral("signature")));
    }

    void walkRejectsHugeBaseCount() {
        // Force CHD.numBaseClasses > 256 — sanity-check guard.
        QByteArray bad = m_data;
        uint32_t huge = 9999;
        // CHD is at imageBase + 0x1400; numBaseClasses is at +0x08.
        std::memcpy(bad.data() + kImageBase + 0x1400 + 0x08, &huge, 4);

        BufferProvider prov(bad, QStringLiteral("bad"));
        auto info = walkRtti(prov, kImageBase + 0x1000);
        QVERIFY(!info.ok);
        QVERIFY(info.error.contains(QStringLiteral("unreasonably")));
    }

    void textReportFormat() {
        BufferProvider prov(m_data, QStringLiteral("synthetic"));
        auto info = walkRtti(prov, kImageBase + 0x1000);
        QVERIFY(info.ok);
        // Report builder lives in rttibrowser.h — we don't include it here
        // (that pulls Qt::Widgets). Instead verify the RttiInfo has the
        // fields a caller would format.
        QVERIFY(!info.demangledName.isEmpty());
        QVERIFY(info.vtableAddress != 0);
        QVERIFY(info.completeLocator != 0);
    }

    // ── Real-binary smoke test ──
    // combase.dll on Windows is a great RTTI candidate: rich C++ COM
    // internals, MSVC-compiled, RTTI not stripped. We don't ship it as
    // a fixture (size + licensing) but on Windows we can map it from
    // System32 on the fly. Auto-skipped on non-Windows / when the file
    // isn't there. The test only verifies the parser doesn't panic on
    // real-world bytes — locating an actual vtable is beyond scope
    // (would need full PE parsing + .rdata search).
    void msvcAbiTagged() {
        BufferProvider prov(m_data, QStringLiteral("synthetic"));
        auto info = walkRtti(prov, kImageBase + 0x1000);
        QVERIFY(info.ok);
        QCOMPARE(info.abi, QStringLiteral("MSVC"));
    }

    // ── Itanium ABI tests ──
    // The synthesizer plants a vtable + type_info + name string into a
    // buffer; the FakeModuleProvider claims a single module covers the
    // RTTI region so findOwningModule resolves. Cross-platform.

    void demangleItaniumSimple() {
        QCOMPARE(demangleItaniumName(QStringLiteral("3Foo")),
                 QStringLiteral("Foo"));
    }
    void demangleItaniumNested() {
        QCOMPARE(demangleItaniumName(QStringLiteral("N3Bar3FooE")),
                 QStringLiteral("Bar::Foo"));
    }
    void demangleItaniumStdShorthand() {
        QString out = demangleItaniumName(QStringLiteral("St9type_info"));
        QVERIFY2(out.endsWith(QStringLiteral("type_info")), qPrintable(out));
    }
    void demangleItaniumPassthrough() {
        QString in = QStringLiteral("plain_text");
        QCOMPARE(demangleItaniumName(in), in);
    }

    void walkSyntheticItanium() {
        ItaniumFixture fx("3Foo");
        auto info = walkRttiItanium(*fx.prov, fx.vtableVa());
        QVERIFY2(info.ok, qPrintable(info.error));
        QCOMPARE(info.abi, QStringLiteral("Itanium"));
        QCOMPARE(info.rawName, QStringLiteral("3Foo"));
        QCOMPARE(info.demangledName, QStringLiteral("Foo"));
        QCOMPARE(info.vtable.size(), 5);
    }

    void walkSyntheticItaniumNested() {
        ItaniumFixture fx("N3Bar3FooE");
        auto info = walkRttiItanium(*fx.prov, fx.vtableVa());
        QVERIFY(info.ok);
        QCOMPARE(info.demangledName, QStringLiteral("Bar::Foo"));
    }

    void rejectsImplausibleOffsetToTop() {
        ItaniumFixture fx("3Foo");
        // Overwrite offset_to_top with junk; the magnitude filter must reject.
        int64_t huge = 0x7FFFFFFFFFFFFFFF;
        std::memcpy(fx.data.data() + ItaniumFixture::kImageBase
                    + 0x1000 - 16, &huge, 8);
        // Re-create provider so it picks up the mutation.
        fx.rebuildProv();
        auto info = walkRttiItanium(*fx.prov, fx.vtableVa());
        QVERIFY(!info.ok);
        QVERIFY(info.error.contains(QStringLiteral("offset_to_top")));
    }

    void rejectsNonItaniumNameString() {
        ItaniumFixture fx("not_a_mangle");  // doesn't start with digit / N / S / etc.
        auto info = walkRttiItanium(*fx.prov, fx.vtableVa());
        QVERIFY(!info.ok);
        QVERIFY(info.error.contains(QStringLiteral("mangle")));
    }

    void smokeTestRealBinary() {
#ifndef _WIN32
        QSKIP("real-binary smoke is Windows-only (Itanium ABI elsewhere)");
#else
        QString path = QStringLiteral("C:/Windows/System32/combase.dll");
        QFileInfo fi(path);
        if (!fi.exists())
            QSKIP("combase.dll not present");
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            QSKIP("could not open combase.dll");
        QByteArray bytes = f.readAll();
        if (bytes.size() < 0x1000)
            QSKIP("combase.dll truncated");

        BufferProvider prov(bytes, QStringLiteral("combase.dll"));
        // Without parsing the PE there's nothing to reliably locate, so
        // probe a few offsets that won't be vtables — we expect graceful
        // failure (ok=false, descriptive error), not a crash.
        for (uint64_t off : {0x1000ULL, 0x10000ULL, 0x100000ULL}) {
            if (off + 0x100 >= (uint64_t)bytes.size()) continue;
            auto info = walkRtti(prov, off, /*ptrSize=*/8, /*maxSlots=*/4);
            // Either ok or a clean error — neither should crash.
            if (!info.ok)
                QVERIFY(!info.error.isEmpty());
        }
#endif
    }

private:
    QByteArray m_buf;   // RTTI bytes only
    QByteArray m_data;  // full address space (zero-padded prefix + RTTI)

    // ── Itanium fixture ──
    // Module-aware BufferProvider for the Itanium synthetic — overrides
    // enumerateModules() so findOwningModule() resolves to our fake module.
    static constexpr uint64_t kItImageBase = 0x10000;
    class ItProv : public BufferProvider {
    public:
        ItProv(QByteArray d, const QString& n) : BufferProvider(std::move(d), n) {}
        QVector<ModuleEntry> enumerateModules() const override {
            return { ModuleEntry{ QStringLiteral("synthetic-itanium"),
                                  QStringLiteral("synthetic-itanium"),
                                  kItImageBase, 0x10000 } };
        }
    };

    // Builds a synthetic Itanium-shape vtable + type_info + mangled-name
    // string in a buffer; provides a module-aware BufferProvider over it.
    struct ItaniumFixture {
        static constexpr uint64_t kImageBase = kItImageBase;
        static constexpr uint32_t kVtableRva = 0x1000;
        static constexpr uint32_t kTiRva     = 0x1100;
        // Name lives well past type_info's two header pointers (tiRva+0,
        // tiRva+8) so we don't trample the name-pointer storage with the
        // string itself.
        static constexpr uint32_t kNameRva   = 0x1180;
        static constexpr uint32_t kTiVtRva   = 0x1200;

        QByteArray data;
        std::unique_ptr<ItProv> prov;

        explicit ItaniumFixture(const char* mangledName) {
            data.resize(kImageBase + 0x10000, 0);
            // offset_to_top = 0 at vtable[-16]
            int64_t z = 0;
            std::memcpy(data.data() + kImageBase + kVtableRva - 16, &z, 8);
            // type_info VA at vtable[-8]
            uint64_t tiVa = kImageBase + kTiRva;
            std::memcpy(data.data() + kImageBase + kVtableRva - 8, &tiVa, 8);
            // 5 method ptrs + null terminator
            for (int i = 0; i < 5; i++) {
                uint64_t mva = kImageBase + 0x100 + (uint64_t)i * 0x10;
                std::memcpy(data.data() + kImageBase + kVtableRva + i * 8, &mva, 8);
            }
            uint64_t zptr = 0;
            std::memcpy(data.data() + kImageBase + kVtableRva + 5 * 8, &zptr, 8);
            // type_info: vtable_ptr at +0, name_ptr at +8
            uint64_t tiVtVa = kImageBase + kTiVtRva;
            std::memcpy(data.data() + kImageBase + kTiRva + 0, &tiVtVa, 8);
            uint64_t nameVa = kImageBase + kNameRva;
            std::memcpy(data.data() + kImageBase + kTiRva + 8, &nameVa, 8);
            // mangled name string
            size_t nlen = std::strlen(mangledName) + 1;
            std::memcpy(data.data() + kImageBase + kNameRva, mangledName, nlen);
            // type_info's vtable head (just needs to be readable)
            uint64_t marker = 0xFEEDFACE;
            std::memcpy(data.data() + kImageBase + kTiVtRva, &marker, 8);

            rebuildProv();
        }
        void rebuildProv() {
            prov = std::make_unique<ItProv>(data, QStringLiteral("synthetic-itanium"));
        }
        uint64_t vtableVa() const { return kImageBase + kVtableRva; }
    };

    // Write `value` into buf at offset `at`.
    template<class T>
    static void writeAt(QByteArray& buf, qsizetype at, T value) {
        std::memcpy(buf.data() + at, &value, sizeof(T));
    }

    static void writeCStr(QByteArray& buf, qsizetype at, const char* s) {
        size_t n = std::strlen(s) + 1;
        std::memcpy(buf.data() + at, s, n);
    }

    // Build the structure described at the top of this file inside `buf`.
    // The parser uses 32-bit RVAs from imageBase for x64 RTTI.
    void layoutSyntheticRtti(QByteArray& buf, uint64_t /*imageBase*/) {
        const uint32_t vtableRva     = 0x1000;
        const uint32_t tdFooRva      = 0x1100;
        const uint32_t tdBarRva      = 0x1200;
        const uint32_t tdBazRva      = 0x1300;
        const uint32_t chdRva        = 0x1400;
        const uint32_t bcaRva        = 0x1500;
        const uint32_t bcdFooRva     = 0x1600;
        const uint32_t bcdBarRva     = 0x1700;
        const uint32_t bcdBazRva     = 0x1800;
        const uint32_t colRva        = 0x1900;

        // ── Vtable: at vtable[-1] (i.e. 8 bytes before vtableRva) we
        //    store the absolute VA of the COL.  Slots 0..4 are method
        //    pointers; slot 5 is null (terminator for our walker).
        uint64_t colVa = kImageBase + colRva;
        writeAt<uint64_t>(buf, vtableRva - 8, colVa);
        for (int i = 0; i < 5; i++) {
            uint64_t methodVa = kImageBase + 0x100 + (uint64_t)i * 0x10;
            writeAt<uint64_t>(buf, vtableRva + (qsizetype)i * 8, methodVa);
        }
        writeAt<uint64_t>(buf, vtableRva + 5 * 8, (uint64_t)0);

        // ── TypeDescriptors: layout is
        //    +0x00 ptr (vtable of type_info) — irrelevant
        //    +0x08 ptr spare
        //    +0x10 char name[]
        auto writeTd = [&](uint32_t rva, const char* name) {
            writeAt<uint64_t>(buf, rva + 0,  0xDEADBEEF);
            writeAt<uint64_t>(buf, rva + 8,  0);
            writeCStr(buf, rva + 16, name);
        };
        writeTd(tdFooRva, ".?AVFoo@@");
        writeTd(tdBarRva, ".?AVBar@@");
        writeTd(tdBazRva, ".?AVBaz@@");

        // ── CHD: 3 bases, pBaseClassArray RVA → bcaRva
        writeAt<uint32_t>(buf, chdRva + 0x00, 0);          // signature
        writeAt<uint32_t>(buf, chdRva + 0x04, 0);          // attributes
        writeAt<uint32_t>(buf, chdRva + 0x08, 3);          // numBaseClasses
        writeAt<uint32_t>(buf, chdRva + 0x0C, bcaRva);     // pBaseClassArray RVA

        // ── BCA: array of 3 BCD RVAs
        writeAt<uint32_t>(buf, bcaRva + 0,  bcdFooRva);
        writeAt<uint32_t>(buf, bcaRva + 4,  bcdBarRva);
        writeAt<uint32_t>(buf, bcaRva + 8,  bcdBazRva);

        // ── BCDs: only +0x00 (pTypeDescriptor RVA) matters for our parser
        writeAt<uint32_t>(buf, bcdFooRva + 0, tdFooRva);
        writeAt<uint32_t>(buf, bcdBarRva + 0, tdBarRva);
        writeAt<uint32_t>(buf, bcdBazRva + 0, tdBazRva);

        // ── COL:
        //    +0x00 sig=0 (or 1; both accepted)
        //    +0x04 offset=0
        //    +0x08 cdOffset=0
        //    +0x0C pTypeDescriptor RVA
        //    +0x10 pClassHierarchy RVA
        //    +0x14 pSelf RVA (used as fallback image-base hint)
        writeAt<uint32_t>(buf, colRva + 0x00, 1);
        writeAt<uint32_t>(buf, colRva + 0x04, 0);
        writeAt<uint32_t>(buf, colRva + 0x08, 0);
        writeAt<uint32_t>(buf, colRva + 0x0C, tdFooRva);
        writeAt<uint32_t>(buf, colRva + 0x10, chdRva);
        writeAt<uint32_t>(buf, colRva + 0x14, (uint32_t)kImageBase);
    }
};

QTEST_MAIN(TestRtti)
#include "test_rtti.moc"
