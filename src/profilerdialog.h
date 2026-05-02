#pragma once
#include <QDialog>
#include <QVector>
#include <QString>

class QTableWidget;
class QCheckBox;
class QLabel;

namespace rcx {

struct ProfileStats;

// Live performance profiler view. Top half: horizontal bar chart of the
// 15 hottest functions by total time. Bottom half: full table with name /
// count / total / mean / min / max / last. Auto-refreshes at ~2 Hz while
// profiling is enabled. Reset clears all aggregated data.
class ProfilerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProfilerDialog(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;

private slots:
    void refreshData();
    void onEnabledToggled(bool on);
    void onReset();

private:
    class BarChart;        // forward; defined in .cpp
    BarChart*       m_chart      = nullptr;
    QTableWidget*   m_table      = nullptr;
    QCheckBox*      m_enableBox  = nullptr;
    QLabel*         m_summary    = nullptr;
    QTimer*         m_timer      = nullptr;
    int             m_lastRowCount = 0;  // resize cols only when row count changes
    void rebuildChart(const QVector<QPair<QString, ProfileStats>>& sorted);
    void rebuildTable(const QVector<QPair<QString, ProfileStats>>& sorted);
    void rebuildSummary(const QVector<QPair<QString, ProfileStats>>& sorted);
};

} // namespace rcx
