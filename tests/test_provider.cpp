#include <QTest>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <QElapsedTimer>
#include <future>
#include <cstring>
#include "providers/provider.h"
#include "providers/buffer_provider.h"
#include "providers/null_provider.h"
#include "providers/file_provider.h"
#include "sparse_file.h"

using namespace rcx;

class TestProvider : public QObject {
    Q_OBJECT

private slots:

    void fileProvider_readsAndEditsAcrossPagesWithoutChangingOriginal() {
        QTemporaryFile file;
        QVERIFY(file.open());
        QByteArray original(200000, '\0');
        for (int i = 0; i < original.size(); ++i) original[i] = char(i % 251);
        QCOMPARE(file.write(original), qint64(original.size()));
        file.flush();
        const auto provider = FileProvider::open(file.fileName());
        QVERIFY(provider);
        QCOMPARE(provider->byteSize(), uint64_t(original.size()));
        QCOMPARE(provider->readBytes(65530, 25), original.mid(65530, 25));
        QCOMPARE(provider->readBytes(199999, 1), original.right(1));
        QVERIFY(!provider->isReadable(UINT64_MAX, 1));
        QVERIFY(!provider->isReadable(0, -1));
        QVERIFY(!provider->isReadable(200000, 1));
        QVERIFY(provider->writeBytes(4090, QByteArray(20, 'X')));
        QByteArray edited = original.mid(4080, 40);
        edited.replace(10, 20, QByteArray(20, 'X'));
        QCOMPARE(provider->readBytes(4080, 40), edited);
        QVERIFY(file.seek(0));
        QCOMPARE(file.readAll(), original);
        QVERIFY(provider->cachedBytes() <= 65536);

        auto reader = [provider, original](int seed) {
            for (int i = 0; i < 100; ++i) {
                const int offset = 70000 + ((i * 997 + seed) % 100000);
                if (provider->readBytes(offset, 1024) != original.mid(offset, 1024)) return false;
            }
            return true;
        };
        auto a = std::async(std::launch::async, reader, 13);
        auto b = std::async(std::launch::async, reader, 47);
        QVERIFY(a.get());
        QVERIFY(b.get());
    }

    void fileProvider_64GiBSparseFile() {
        QTemporaryFile file;
        QVERIFY(file.open());
        constexpr qint64 length = (qint64(1) << 36) + 123;
        if (!resizeSparseTestFile(file, length)) QSKIP("Filesystem does not support sparse test files");
        const QByteArray marker("large-file-sentinel");
        const QVector<qint64> offsets = {0, (qint64(1) << 31) - 5,
            (qint64(1) << 32) + 19, length - marker.size()};
        for (qint64 offset : offsets) {
            QVERIFY(file.seek(offset));
            QCOMPARE(file.write(marker), qint64(marker.size()));
        }
        file.flush();
        QElapsedTimer timer;
        timer.start();
        const auto provider = FileProvider::open(file.fileName());
        QVERIFY(provider);
        QCOMPARE(provider->byteSize(), uint64_t(length));
        QCOMPARE(provider->size(), INT_MAX);
        QCOMPARE(provider->cachedBytes(), 0);
        QCOMPARE(provider->enumerateRegions().first().size, uint64_t(length));
        for (qint64 offset : offsets)
            QCOMPARE(provider->readBytes(uint64_t(offset), marker.size()), marker);
        QVERIFY(provider->writeBytes(uint64_t(length - 2), QByteArray("XY")));
        QCOMPARE(provider->readBytes(uint64_t(length - 2), 2), QByteArray("XY"));
        QVERIFY(file.seek(length - marker.size()));
        QCOMPARE(file.read(marker.size()), marker);
        QVERIFY(!provider->isReadable(uint64_t(length), 1));
        QVERIFY(provider->cachedBytes() <= 65536);
        qInfo() << "64 GiB sparse file open/read/edit:" << timer.elapsed() << "ms, cache:" << provider->cachedBytes();
    }

    // ---------------------------------------------------------------
    // NullProvider
    // ---------------------------------------------------------------

    void nullProvider_isNotValid() {
        NullProvider p;
        QVERIFY(!p.isValid());
        QCOMPARE(p.size(), 0);
    }

    void nullProvider_readFails() {
        NullProvider p;
        uint8_t buf = 0xFF;
        QVERIFY(!p.read(0, &buf, 1));
        QCOMPARE(buf, (uint8_t)0xFF); // buf unchanged on failure
    }

    void nullProvider_readU8ReturnsZero() {
        NullProvider p;
        QCOMPARE(p.readU8(0), (uint8_t)0);
    }

    void nullProvider_readBytesReturnsZeroed() {
        NullProvider p;
        QByteArray b = p.readBytes(0, 4);
        QCOMPARE(b.size(), 4);
        QCOMPARE(b, QByteArray(4, '\0'));
    }

    void nullProvider_isNotWritable() {
        NullProvider p;
        QVERIFY(!p.isWritable());
    }

    void nullProvider_nameIsEmpty() {
        NullProvider p;
        QVERIFY(p.name().isEmpty());
    }

    void nullProvider_getSymbolReturnsEmpty() {
        NullProvider p;
        QVERIFY(p.getSymbol(0x7FF00000).isEmpty());
    }

    // ---------------------------------------------------------------
    // BufferProvider -- construction
    // ---------------------------------------------------------------

    void buffer_emptyIsNotValid() {
        BufferProvider p(QByteArray{});
        QVERIFY(!p.isValid());
        QCOMPARE(p.size(), 0);
    }

    void buffer_nonEmptyIsValid() {
        BufferProvider p(QByteArray(16, '\0'));
        QVERIFY(p.isValid());
        QCOMPARE(p.size(), 16);
    }

    void buffer_nameFromConstructor() {
        BufferProvider p(QByteArray(4, '\0'), "dump.bin");
        QCOMPARE(p.name(), QStringLiteral("dump.bin"));
        QCOMPARE(p.kind(), QStringLiteral("File"));
    }

    void buffer_nameEmptyByDefault() {
        BufferProvider p(QByteArray(4, '\0'));
        QVERIFY(p.name().isEmpty());
    }

    // ---------------------------------------------------------------
    // BufferProvider -- reading typed values
    // ---------------------------------------------------------------

    void buffer_readU8() {
        QByteArray d(4, '\0');
        d[0] = (char)0xAB;
        BufferProvider p(d);
        QCOMPARE(p.readU8(0), (uint8_t)0xAB);
    }

    void buffer_readU16_littleEndian() {
        QByteArray d(4, '\0');
        d[0] = (char)0x34; d[1] = (char)0x12;
        BufferProvider p(d);
        QCOMPARE(p.readU16(0), (uint16_t)0x1234);
    }

    void buffer_readU32() {
        QByteArray d(8, '\0');
        uint32_t val = 0xDEADBEEF;
        std::memcpy(d.data(), &val, 4);
        BufferProvider p(d);
        QCOMPARE(p.readU32(0), (uint32_t)0xDEADBEEF);
    }

    void buffer_readU64() {
        QByteArray d(16, '\0');
        uint64_t val = 0x0102030405060708ULL;
        std::memcpy(d.data() + 4, &val, 8);
        BufferProvider p(d);
        QCOMPARE(p.readU64(4), val);
    }

    void buffer_readF32() {
        QByteArray d(4, '\0');
        float val = 3.14f;
        std::memcpy(d.data(), &val, 4);
        BufferProvider p(d);
        QCOMPARE(p.readF32(0), val);
    }

    void buffer_readF64() {
        QByteArray d(8, '\0');
        double val = 2.71828;
        std::memcpy(d.data(), &val, 8);
        BufferProvider p(d);
        QCOMPARE(p.readF64(0), val);
    }

    void buffer_readAs_customStruct() {
        struct Pair { uint16_t a; uint16_t b; };
        QByteArray d(4, '\0');
        Pair orig{0x1111, 0x2222};
        std::memcpy(d.data(), &orig, 4);
        BufferProvider p(d);
        Pair result = p.readAs<Pair>(0);
        QCOMPARE(result.a, (uint16_t)0x1111);
        QCOMPARE(result.b, (uint16_t)0x2222);
    }

    // ---------------------------------------------------------------
    // BufferProvider -- readBytes
    // ---------------------------------------------------------------

    void buffer_readBytes_full() {
        QByteArray d("Hello, World!", 13);
        BufferProvider p(d);
        QCOMPARE(p.readBytes(0, 5), QByteArray("Hello"));
    }

    void buffer_readBytes_offset() {
        QByteArray d("ABCDEFGH", 8);
        BufferProvider p(d);
        QCOMPARE(p.readBytes(4, 4), QByteArray("EFGH"));
    }

    void buffer_readBytes_pastEnd() {
        QByteArray d(4, 'X');
        BufferProvider p(d);
        QByteArray result = p.readBytes(2, 8);
        // read fails (past end), returns zeroed buffer
        QCOMPARE(result.size(), 8);
        QCOMPARE(result, QByteArray(8, '\0'));
    }

    void buffer_readBytes_zeroLen() {
        BufferProvider p(QByteArray(4, '\0'));
        QByteArray result = p.readBytes(0, 0);
        QCOMPARE(result.size(), 0);
    }

    // ---------------------------------------------------------------
    // BufferProvider -- isReadable boundary checks
    // ---------------------------------------------------------------

    void buffer_isReadable_withinBounds() {
        BufferProvider p(QByteArray(16, '\0'));
        QVERIFY(p.isReadable(0, 16));
        QVERIFY(p.isReadable(15, 1));
        QVERIFY(p.isReadable(0, 0));
    }

    void buffer_isReadable_outOfBounds() {
        BufferProvider p(QByteArray(16, '\0'));
        QVERIFY(!p.isReadable(0, 17));
        QVERIFY(!p.isReadable(16, 1));
        QVERIFY(!p.isReadable(100, 1));
    }

    void buffer_isReadable_zeroSizeProvider() {
        BufferProvider p(QByteArray{});
        QVERIFY(!p.isReadable(0, 1));
        QVERIFY(p.isReadable(0, 0)); // zero-len read always ok
    }

    // ---------------------------------------------------------------
    // BufferProvider -- writing
    // ---------------------------------------------------------------

    void buffer_isWritable() {
        BufferProvider p(QByteArray(4, '\0'));
        QVERIFY(p.isWritable());
    }

    void buffer_writeBytes() {
        QByteArray d(8, '\0');
        BufferProvider p(d);
        QByteArray payload("\xAA\xBB\xCC\xDD", 4);
        QVERIFY(p.writeBytes(2, payload));
        QCOMPARE(p.readU8(2), (uint8_t)0xAA);
        QCOMPARE(p.readU8(5), (uint8_t)0xDD);
    }

    void buffer_write_pastEndFails() {
        BufferProvider p(QByteArray(4, '\0'));
        QByteArray big(8, 'X');
        QVERIFY(!p.writeBytes(0, big));
    }

    void buffer_write_thenRead() {
        QByteArray d(8, '\0');
        BufferProvider p(d);
        uint32_t val = 0x12345678;
        QVERIFY(p.write(0, &val, sizeof(val)));
        QCOMPARE(p.readU32(0), (uint32_t)0x12345678);
    }

    // ---------------------------------------------------------------
    // BufferProvider -- fromFile
    // ---------------------------------------------------------------

    void buffer_fromFile_nonexistent() {
        auto p = BufferProvider::fromFile("/tmp/__rcx_test_nonexistent_file__");
        QVERIFY(!p.isValid());
        QCOMPARE(p.size(), 0);
    }

    void buffer_fromFile_valid() {
        // Write a temp file, read it back
        QString path = QDir::tempPath() + "/rcx_test_buffer_provider.bin";
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(64, '\xAB'));
        }
        auto p = BufferProvider::fromFile(path);
        QVERIFY(p.isValid());
        QCOMPARE(p.size(), 64);
        QCOMPARE(p.readU8(0), (uint8_t)0xAB);
        QCOMPARE(p.name(), QStringLiteral("rcx_test_buffer_provider.bin"));
        QFile::remove(path);
    }

    // ---------------------------------------------------------------
    // Polymorphism -- unique_ptr<Provider> usage
    // ---------------------------------------------------------------

    void polymorphic_nullToBuffer() {
        std::unique_ptr<Provider> prov = std::make_unique<NullProvider>();
        QVERIFY(!prov->isValid());
        QVERIFY(prov->name().isEmpty());

        // Switch to buffer
        QByteArray d(8, '\0');
        uint64_t val = 0xCAFEBABE;
        std::memcpy(d.data(), &val, sizeof(val));
        prov = std::make_unique<BufferProvider>(d, "test.bin");

        QVERIFY(prov->isValid());
        QCOMPARE(prov->readU64(0), (uint64_t)0xCAFEBABE);
        QCOMPARE(prov->name(), QStringLiteral("test.bin"));
        QCOMPARE(prov->kind(), QStringLiteral("File"));
        QVERIFY(prov->getSymbol(0x1000).isEmpty());
    }

    // ---------------------------------------------------------------
    // getSymbol -- base class returns empty
    // ---------------------------------------------------------------

    void buffer_getSymbol_alwaysEmpty() {
        BufferProvider p(QByteArray(64, '\0'), "test.bin");
        QVERIFY(p.getSymbol(0).isEmpty());
        QVERIFY(p.getSymbol(0x7FF00000).isEmpty());
    }
};

QTEST_MAIN(TestProvider)
#include "test_provider.moc"
