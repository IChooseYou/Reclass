#include "profilerdialog.h"
#include "profiler.h"
#include "themes/thememanager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPaintEvent>
#include <QTableWidget>
#include <QHeaderView>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QFontMetrics>
#include <QClipboard>
#include <QApplication>
#include <QSettings>
#include <QShowEvent>
#include <QHideEvent>
#include <algorithm>

namespace rcx {

// ── BarChart ──
// Custom QWidget that paints horizontal bars proportional to total time.
// Top entry's total maps to full bar width; others scale linearly.
class ProfilerDialog::BarChart : public QWidget {
public:
    QVector<QPair<QString, ProfileStats>> rows;  // already sorted desc by totalNs

    explicit BarChart(QWidget* parent) : QWidget(parent) {
        setMinimumHeight(220);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const auto& t = ThemeManager::instance().current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), t.background);

        if (rows.isEmpty()) {
            p.setPen(t.textDim);
            p.drawText(rect(), Qt::AlignCenter,
                       QStringLiteral("(no samples — enable profiling above)"));
            return;
        }

        QFont labelFont = font();
        labelFont.setPointSize(qMax(8, font().pointSize() - 1));
        p.setFont(labelFont);
        QFontMetrics fm(labelFont);
        int barH    = qMax(14, fm.height() + 4);
        int gap     = 2;
        int leftPad = 6;
        int rightPad = 6;
        int labelW = 0;
        for (const auto& r : rows)
            labelW = qMax(labelW, fm.horizontalAdvance(r.first));
        labelW = qMin(labelW, width() / 3);

        int valueW = fm.horizontalAdvance(QStringLiteral("000.00 ms x000000"));
        int barX   = leftPad + labelW + 8;
        int barMaxW = qMax(40, width() - barX - rightPad - valueW - 8);

        qint64 maxTotal = rows.first().second.totalNs;
        if (maxTotal <= 0) maxTotal = 1;

        int y = 4;
        for (int i = 0; i < rows.size() && y + barH <= height(); ++i) {
            const auto& [name, s] = rows[i];

            // Label (truncate with ellipsis if needed)
            QRect labelR(leftPad, y, labelW, barH);
            QString shown = fm.elidedText(name, Qt::ElideRight, labelW);
            p.setPen(t.text);
            p.drawText(labelR, Qt::AlignVCenter | Qt::AlignLeft, shown);

            // Bar — color cycles through accents based on rank
            int barW = (int)((qint64)barMaxW * s.totalNs / maxTotal);
            QColor barColor = (i == 0) ? t.indHeatHot
                            : (i <= 2) ? t.indHeatWarm
                            : (i <= 5) ? t.indHeatCold
                            : t.indHoverSpan;
            p.fillRect(QRect(barX, y + 2, barW, barH - 4), barColor);

            // Value text after bar
            double totalMs = s.totalNs / 1.0e6;
            QString valueText = QStringLiteral("%1 ms  ×%2")
                .arg(totalMs, 0, 'f', 2)
                .arg(s.count);
            QRect valR(barX + barW + 6, y, width() - (barX + barW + 6) - rightPad, barH);
            p.setPen(t.textDim);
            p.drawText(valR, Qt::AlignVCenter | Qt::AlignLeft, valueText);

            y += barH + gap;
        }
    }
};

// ── Dialog ──

ProfilerDialog::ProfilerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Performance Profiler"));
    resize(820, 640);

    const auto& t = ThemeManager::instance().current();
    {
        QPalette pal = palette();
        pal.setColor(QPalette::Window, t.background);
        pal.setColor(QPalette::WindowText, t.text);
        setPalette(pal);
        setAutoFillBackground(true);
    }

    QSettings s("Reclass", "Reclass");
    QFont monoFont(s.value("font", "JetBrains Mono").toString(), 10);
    monoFont.setFixedPitch(true);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(10, 10, 10, 10);
    lay->setSpacing(8);

    // Top control row: enable toggle + reset + summary
    {
        auto* row = new QHBoxLayout;

        m_enableBox = new QCheckBox(QStringLiteral("Profile compose / refresh / applyDocument"));
        m_enableBox->setChecked(Profiler::instance().isEnabled());
        m_enableBox->setStyleSheet(QStringLiteral("color: %1;").arg(t.text.name()));
        connect(m_enableBox, &QCheckBox::toggled, this, &ProfilerDialog::onEnabledToggled);
        row->addWidget(m_enableBox);

        row->addStretch();

        m_summary = new QLabel(QStringLiteral("(no data)"));
        m_summary->setStyleSheet(QStringLiteral("color: %1;").arg(t.textDim.name()));
        m_summary->setFont(monoFont);
        row->addWidget(m_summary);

        auto* resetBtn = new QPushButton(QStringLiteral("Reset"));
        resetBtn->setCursor(Qt::PointingHandCursor);
        connect(resetBtn, &QPushButton::clicked, this, &ProfilerDialog::onReset);
        row->addWidget(resetBtn);

        auto* copyBtn = new QPushButton(QStringLiteral("Copy CSV"));
        copyBtn->setCursor(Qt::PointingHandCursor);
        connect(copyBtn, &QPushButton::clicked, this, [this]() {
            auto snap = Profiler::instance().snapshot();
            QStringList lines;
            lines << QStringLiteral("name,count,total_ms,mean_us,min_us,max_us,last_us");
            for (auto it = snap.begin(); it != snap.end(); ++it) {
                const auto& s = it.value();
                double mean = s.count ? double(s.totalNs) / s.count : 0;
                lines << QStringLiteral("%1,%2,%3,%4,%5,%6,%7")
                    .arg(it.key())
                    .arg(s.count)
                    .arg(s.totalNs / 1.0e6, 0, 'f', 4)
                    .arg(mean / 1.0e3, 0, 'f', 3)
                    .arg(s.minNs / 1.0e3, 0, 'f', 3)
                    .arg(s.maxNs / 1.0e3, 0, 'f', 3)
                    .arg(s.lastNs / 1.0e3, 0, 'f', 3);
            }
            QApplication::clipboard()->setText(lines.join('\n'));
        });
        row->addWidget(copyBtn);

        lay->addLayout(row);
    }

    // Chart
    m_chart = new BarChart(this);
    m_chart->setStyleSheet(QStringLiteral(
        "background: %1; border: 1px solid %2;")
        .arg(t.background.name(), t.border.name()));
    lay->addWidget(m_chart, /*stretch=*/2);

    // Table
    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels(
        {"Function", "Count", "Total (ms)", "Mean (µs)", "Min (µs)", "Max (µs)", "Last (µs)"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setFont(monoFont);
    m_table->setStyleSheet(QStringLiteral(
        "QTableWidget { background: %1; color: %2; border: 1px solid %3; }"
        "QTableWidget::item { padding: 2px 6px; }"
        "QHeaderView::section { background: %4; color: %2; border: 0;"
        "  padding: 4px 6px; border-bottom: 1px solid %3; }")
        .arg(t.background.name(), t.text.name(), t.border.name(), t.backgroundAlt.name()));
    lay->addWidget(m_table, /*stretch=*/3);

    // Auto-refresh — started/stopped in showEvent/hideEvent so a hidden
    // dialog doesn't keep paying the snapshot+repaint cost.
    m_timer = new QTimer(this);
    m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, &ProfilerDialog::refreshData);
    refreshData();
}

void ProfilerDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    if (m_timer && !m_timer->isActive()) m_timer->start();
    refreshData();
}

void ProfilerDialog::hideEvent(QHideEvent* e) {
    if (m_timer) m_timer->stop();
    QDialog::hideEvent(e);
}

void ProfilerDialog::onEnabledToggled(bool on) {
    Profiler::instance().setEnabled(on);
    refreshData();
}

void ProfilerDialog::onReset() {
    Profiler::instance().reset();
    refreshData();
}

void ProfilerDialog::refreshData() {
    auto snap = Profiler::instance().snapshot();
    QVector<QPair<QString, ProfileStats>> sorted;
    sorted.reserve(snap.size());
    for (auto it = snap.begin(); it != snap.end(); ++it)
        sorted.emplaceBack(it.key(), it.value());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.totalNs > b.second.totalNs; });

    rebuildChart(sorted);
    rebuildTable(sorted);
    rebuildSummary(sorted);
}

void ProfilerDialog::rebuildChart(const QVector<QPair<QString, ProfileStats>>& sorted) {
    m_chart->rows = sorted.mid(0, 15);  // top 15
    m_chart->update();
}

void ProfilerDialog::rebuildTable(const QVector<QPair<QString, ProfileStats>>& sorted) {
    const bool rowCountChanged = (sorted.size() != m_lastRowCount);
    if (rowCountChanged) {
        m_table->setRowCount(sorted.size());
        // Allocate items for any newly-added rows; existing rows keep their items.
        for (int i = m_lastRowCount; i < sorted.size(); ++i) {
            for (int col = 0; col < 7; ++col) {
                auto* item = new QTableWidgetItem;
                Qt::Alignment align = (col == 0) ? Qt::AlignLeft : Qt::AlignRight;
                item->setTextAlignment(align | Qt::AlignVCenter);
                m_table->setItem(i, col, item);
            }
        }
        m_lastRowCount = sorted.size();
    }
    for (int i = 0; i < sorted.size(); ++i) {
        const auto& [name, s] = sorted[i];
        double mean = s.count ? double(s.totalNs) / s.count : 0;
        m_table->item(i, 0)->setText(name);
        m_table->item(i, 1)->setText(QString::number(s.count));
        m_table->item(i, 2)->setText(QString::number(s.totalNs / 1.0e6, 'f', 3));
        m_table->item(i, 3)->setText(QString::number(mean / 1.0e3, 'f', 2));
        m_table->item(i, 4)->setText(QString::number(s.minNs / 1.0e3, 'f', 2));
        m_table->item(i, 5)->setText(QString::number(s.maxNs / 1.0e3, 'f', 2));
        m_table->item(i, 6)->setText(QString::number(s.lastNs / 1.0e3, 'f', 2));
    }
    if (rowCountChanged) {
        m_table->resizeColumnsToContents();
        m_table->horizontalHeader()->setStretchLastSection(true);
    }
}

void ProfilerDialog::rebuildSummary(const QVector<QPair<QString, ProfileStats>>& sorted) {
    if (sorted.isEmpty()) {
        m_summary->setText(QStringLiteral("(no data)"));
        return;
    }
    qint64 grandTotal = 0;
    int    grandCount = 0;
    for (const auto& [n, s] : sorted) {
        grandTotal += s.totalNs;
        grandCount += s.count;
    }
    m_summary->setText(QStringLiteral("%1 buckets · %2 samples · %3 ms total")
        .arg(sorted.size())
        .arg(grandCount)
        .arg(grandTotal / 1.0e6, 0, 'f', 2));
}

} // namespace rcx
