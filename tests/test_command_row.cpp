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
        return QStringLiteral("source\u25BE");
    return QStringLiteral("'%1'\u25BE").arg(provName);
}

static QString buildCommandRow(const Provider& prov, uint64_t baseAddress) {
    QString src = buildSourceLabel(prov);
    QString addr = QStringLiteral("0x") +
        QString::number(baseAddress, 16).toUpper();
    return QStringLiteral("   %1 \u00B7 %2").arg(src, addr);
}

// -- Replicate commandRowSrcSpan for testing
struct TestColumnSpan {
    int start = 0;
    int end = 0;
    bool valid = false;
};

static TestColumnSpan commandRowSrcSpan(const QString& lineText) {
    int idx = lineText.indexOf(QStringLiteral(" \u00B7"));
    if (idx < 0) return {};
    int start = 0;
    while (start < idx && !lineText[start].isLetterOrNumber()
           && lineText[start] != '<' && lineText[start] != '\'') start++;
    if (start >= idx) return {};
    // Exclude trailing ▾ from the editable span
    int end = idx;
    while (end > start && lineText[end - 1] == QChar(0x25BE)) end--;
    if (end <= start) return {};
    return {start, end, true};
}

class TestCommandRow : public QObject {
    Q_OBJECT

private slots:

    // ---------------------------------------------------------------
    // Source label text
    // ---------------------------------------------------------------

    void label_nullProvider_showsSelectSource() {
        NullProvider p;
        QCOMPARE(buildSourceLabel(p), QStringLiteral("source\u25BE"));
    }

    void label_bufferNoName_showsSelectSource() {
        // BufferProvider with empty name also triggers source▾
        BufferProvider p(QByteArray(4, '\0'));
        QCOMPARE(buildSourceLabel(p), QStringLiteral("source\u25BE"));
    }

    void label_bufferWithName_showsFileAndName() {
        BufferProvider p(QByteArray(4, '\0'), "dump.bin");
        QCOMPARE(buildSourceLabel(p), QStringLiteral("'dump.bin'\u25BE"));
    }

    // ---------------------------------------------------------------
    // Full command row text
    // ---------------------------------------------------------------

    void row_nullProvider() {
        NullProvider p;
        QString row = buildCommandRow(p, 0);
        QCOMPARE(row, QStringLiteral("   source\u25BE \u00B7 0x0"));
    }

    void row_fileProvider() {
        BufferProvider p(QByteArray(4, '\0'), "test.bin");
        QString row = buildCommandRow(p, 0x140000000ULL);
        QCOMPARE(row, QStringLiteral("   'test.bin'\u25BE \u00B7 0x140000000"));
    }

    // ---------------------------------------------------------------
    // Source span parsing
    // ---------------------------------------------------------------

    void span_selectSource() {
        QString row = buildCommandRow(NullProvider{}, 0);
        auto span = commandRowSrcSpan(row);
        QVERIFY(span.valid);
        QString extracted = row.mid(span.start, span.end - span.start);
        QCOMPARE(extracted, QStringLiteral("source"));
    }

    void span_fileProvider() {
        BufferProvider p(QByteArray(4, '\0'), "dump.bin");
        QString row = buildCommandRow(p, 0x140000000ULL);
        auto span = commandRowSrcSpan(row);
        QVERIFY(span.valid);
        QString extracted = row.mid(span.start, span.end - span.start);
        QCOMPARE(extracted, QStringLiteral("'dump.bin'"));
    }

    void span_processProvider_simulated() {
        // Simulate a process provider without needing Windows APIs
        // by building the string directly
        QString row = QStringLiteral("   'notepad.exe'\u25BE \u00B7 0x7FF600000000");
        auto span = commandRowSrcSpan(row);
        QVERIFY(span.valid);
        QString extracted = row.mid(span.start, span.end - span.start);
        QCOMPARE(extracted, QStringLiteral("'notepad.exe'"));
    }

    // ---------------------------------------------------------------
    // Provider switching simulation
    // ---------------------------------------------------------------

    void switching_nullToFileToProcess() {
        // Start with NullProvider
        std::unique_ptr<Provider> prov = std::make_unique<NullProvider>();
        QCOMPARE(buildSourceLabel(*prov), QStringLiteral("source\u25BE"));

        // User loads a file
        prov = std::make_unique<BufferProvider>(QByteArray(64, '\0'), "game.exe");
        QCOMPARE(buildSourceLabel(*prov), QStringLiteral("'game.exe'\u25BE"));

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
