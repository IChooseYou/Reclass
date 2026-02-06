#include <QTest>
#include <QString>
#include <memory>
#include "providers/provider.h"
#include "providers/buffer_provider.h"
#include "providers/null_provider.h"

using namespace rcx;

// -- Replicate the label-building logic from updateCommandRow so we can test it
//    without needing a full RcxController/RcxDocument/RcxEditor stack.

static QString buildSourceLabel(const Provider& prov) {
    QString provName = prov.name();
    if (provName.isEmpty())
        return QStringLiteral("<Select Source>");
    return QStringLiteral("%1 '%2'").arg(prov.kind(), provName);
}

static QString buildCommandRow(const Provider& prov, uint64_t baseAddress) {
    QString src = buildSourceLabel(prov);
    QString addr = QStringLiteral("0x") +
        QString::number(baseAddress, 16).toUpper();
    return QStringLiteral("   %1 Address: %2").arg(src, addr);
}

// -- Replicate commandRowSrcSpan for testing
struct TestColumnSpan {
    int start = 0;
    int end = 0;
    bool valid = false;
};

static TestColumnSpan commandRowSrcSpan(const QString& lineText) {
    int idx = lineText.indexOf(QStringLiteral(" Address: "));
    if (idx < 0) return {};
    int start = 0;
    while (start < idx && !lineText[start].isLetterOrNumber()
           && lineText[start] != '<') start++;
    if (start >= idx) return {};
    return {start, idx, true};
}

class TestCommandRow : public QObject {
    Q_OBJECT

private slots:

    // ---------------------------------------------------------------
    // Source label text
    // ---------------------------------------------------------------

    void label_nullProvider_showsSelectSource() {
        NullProvider p;
        QCOMPARE(buildSourceLabel(p), QStringLiteral("<Select Source>"));
    }

    void label_bufferNoName_showsSelectSource() {
        // BufferProvider with empty name also triggers <Select Source>
        BufferProvider p(QByteArray(4, '\0'));
        QCOMPARE(buildSourceLabel(p), QStringLiteral("<Select Source>"));
    }

    void label_bufferWithName_showsFileAndName() {
        BufferProvider p(QByteArray(4, '\0'), "dump.bin");
        QCOMPARE(buildSourceLabel(p), QStringLiteral("File 'dump.bin'"));
    }

    // ---------------------------------------------------------------
    // Full command row text
    // ---------------------------------------------------------------

    void row_nullProvider() {
        NullProvider p;
        QString row = buildCommandRow(p, 0);
        QCOMPARE(row, QStringLiteral("   <Select Source> Address: 0x0"));
    }

    void row_fileProvider() {
        BufferProvider p(QByteArray(4, '\0'), "test.bin");
        QString row = buildCommandRow(p, 0x140000000ULL);
        QCOMPARE(row, QStringLiteral("   File 'test.bin' Address: 0x140000000"));
    }

    // ---------------------------------------------------------------
    // Source span parsing
    // ---------------------------------------------------------------

    void span_selectSource() {
        QString row = buildCommandRow(NullProvider{}, 0);
        auto span = commandRowSrcSpan(row);
        QVERIFY(span.valid);
        QString extracted = row.mid(span.start, span.end - span.start);
        QCOMPARE(extracted, QStringLiteral("<Select Source>"));
    }

    void span_fileProvider() {
        BufferProvider p(QByteArray(4, '\0'), "dump.bin");
        QString row = buildCommandRow(p, 0x140000000ULL);
        auto span = commandRowSrcSpan(row);
        QVERIFY(span.valid);
        QString extracted = row.mid(span.start, span.end - span.start);
        QCOMPARE(extracted, QStringLiteral("File 'dump.bin'"));
    }

    void span_processProvider_simulated() {
        // Simulate a process provider without needing Windows APIs
        // by building the string directly
        QString row = QStringLiteral("   Process 'notepad.exe' Address: 0x7FF600000000");
        auto span = commandRowSrcSpan(row);
        QVERIFY(span.valid);
        QString extracted = row.mid(span.start, span.end - span.start);
        QCOMPARE(extracted, QStringLiteral("Process 'notepad.exe'"));
    }

    // ---------------------------------------------------------------
    // Provider switching simulation
    // ---------------------------------------------------------------

    void switching_nullToFileToProcess() {
        // Start with NullProvider
        std::unique_ptr<Provider> prov = std::make_unique<NullProvider>();
        QCOMPARE(buildSourceLabel(*prov), QStringLiteral("<Select Source>"));

        // User loads a file
        prov = std::make_unique<BufferProvider>(QByteArray(64, '\0'), "game.exe");
        QCOMPARE(buildSourceLabel(*prov), QStringLiteral("File 'game.exe'"));

        // User switches to a "process" -- simulate with a named BufferProvider
        // (ProcessProvider needs Windows, but the label logic is the same)
        prov = std::make_unique<BufferProvider>(QByteArray(64, '\0'), "notepad.exe");
        // BufferProvider kind is "File", but the switching mechanism works the same
        QCOMPARE(prov->kind(), QStringLiteral("File"));
        QCOMPARE(prov->name(), QStringLiteral("notepad.exe"));
    }
};

QTEST_MAIN(TestCommandRow)
#include "test_command_row.moc"
