#ifndef PROCESSPICKER_H
#define PROCESSPICKER_H

#include <QDialog>
#include <QIcon>
#include <cstdint>

namespace Ui {
class ProcessPicker;
}

struct ProcessInfo {
    uint32_t pid;
    QString name;
    QString path;
    QIcon icon;
};

class ProcessPicker : public QDialog
{
    Q_OBJECT

public:
    explicit ProcessPicker(QWidget *parent = nullptr);
    explicit ProcessPicker(const QList<ProcessInfo>& customProcesses, QWidget *parent = nullptr);
    ~ProcessPicker();

    uint32_t selectedProcessId() const;
    QString selectedProcessName() const;

private slots:
    void refreshProcessList();
    void onProcessSelected();
    void filterProcesses(const QString& text);

private:
    void enumerateProcesses();
    void populateTable(const QList<ProcessInfo>& processes);
    void applyFilter();

    Ui::ProcessPicker *ui;
    uint32_t m_selectedPid = 0;
    QString m_selectedName;
    QList<ProcessInfo> m_allProcesses;
    bool m_useCustomList = false;
};

#endif // PROCESSPICKER_H
