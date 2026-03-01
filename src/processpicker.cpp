#include "processpicker.h"
#include "ui_processpicker.h"
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QMessageBox>
#include <QFileInfo>
#include <QPixmap>

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtWin>
#endif
#elif defined(__linux__)
#include <QDir>
#include <QStyle>
#include <QApplication>
#include <unistd.h>
#endif

ProcessPicker::ProcessPicker(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ProcessPicker)
    , m_useCustomList(false)
{
    ui->setupUi(this);
    
    // Configure table
    ui->processTable->setColumnWidth(0, 80);   // PID column - fixed width
    ui->processTable->setColumnWidth(1, 200);  // Name column - fixed width
    ui->processTable->horizontalHeader()->setStretchLastSection(true);  // Path column - fills remaining space
    ui->processTable->setWordWrap(false);  // Disable word wrap for single-line display
    ui->processTable->setTextElideMode(Qt::ElideLeft);  // Elide from left (show end of path)
    
    // Connect signals
    connect(ui->refreshButton, &QPushButton::clicked, this, &ProcessPicker::refreshProcessList);
    connect(ui->processTable, &QTableWidget::itemDoubleClicked, this, &ProcessPicker::onProcessSelected);
    connect(ui->filterEdit, &QLineEdit::textChanged, this, &ProcessPicker::filterProcesses);
    connect(ui->attachButton, &QPushButton::clicked, this, &ProcessPicker::onProcessSelected);
    
    // Initial process enumeration
    refreshProcessList();
}

ProcessPicker::ProcessPicker(const QList<ProcessInfo>& customProcesses, QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ProcessPicker)
    , m_useCustomList(true)
{
    ui->setupUi(this);
    
    // Configure table
    ui->processTable->setColumnWidth(0, 80);
    ui->processTable->setColumnWidth(1, 200);
    ui->processTable->horizontalHeader()->setStretchLastSection(true);
    ui->processTable->setWordWrap(false);
    ui->processTable->setTextElideMode(Qt::ElideLeft);
    
    // Connect signals (no refresh button for custom lists)
    ui->refreshButton->setVisible(false);
    connect(ui->processTable, &QTableWidget::itemDoubleClicked, this, &ProcessPicker::onProcessSelected);
    connect(ui->filterEdit, &QLineEdit::textChanged, this, &ProcessPicker::filterProcesses);
    connect(ui->attachButton, &QPushButton::clicked, this, &ProcessPicker::onProcessSelected);
    
    // Use custom process list
    m_allProcesses = customProcesses;
    applyFilter();
}

ProcessPicker::~ProcessPicker()
{
    delete ui;
}

uint32_t ProcessPicker::selectedProcessId() const
{
    return m_selectedPid;
}

QString ProcessPicker::selectedProcessName() const
{
    return m_selectedName;
}

void ProcessPicker::refreshProcessList()
{
    ui->processTable->clearContents();
    ui->processTable->setRowCount(0);
    m_allProcesses.clear();
    enumerateProcesses();
}

void ProcessPicker::onProcessSelected()
{
    auto* item = ui->processTable->currentItem();
    if (!item) return;
    
    int row = item->row();
    m_selectedPid = ui->processTable->item(row, 0)->data(Qt::EditRole).toUInt();
    // Use original name stored in UserRole (without architecture suffix)
    QVariant origName = ui->processTable->item(row, 1)->data(Qt::UserRole);
    m_selectedName = origName.isValid() ? origName.toString()
                                        : ui->processTable->item(row, 1)->text();
    
    accept();
}

void ProcessPicker::enumerateProcesses()
{
    QList<ProcessInfo> processes;
    
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        QMessageBox::warning(this, "Error", "Failed to enumerate processes.");
        return;
    }
    
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    
    if (Process32FirstW(snapshot, &pe32))
    {
        do
        {
            ProcessInfo info;
            info.pid = pe32.th32ProcessID;
            info.name = QString::fromWCharArray(pe32.szExeFile);
            
            // Try to get full path and extract icon
            // If we can't open a process with PROCESS_QUERY_LIMITED_INFORMATION then
            // we for sure can't access their memory. - Skip in this case
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
            if (hProcess)
            {
                WCHAR path[MAX_PATH];
                DWORD pathLen = MAX_PATH;
                if (QueryFullProcessImageNameW(hProcess, 0, path, &pathLen) ||
                    QueryFullProcessImageNameW(hProcess, PROCESS_NAME_NATIVE, path, &pathLen) ||
                    GetModuleFileNameExW(hProcess, nullptr, path, pathLen))
                {
                    info.path = QString::fromWCharArray(path);
                    
                    // Extract icon from executable
                    SHFILEINFOW sfi = {};
                    if (SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
                        if (sfi.hIcon) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                            info.icon = QIcon(QPixmap::fromImage(QImage::fromHICON(sfi.hIcon)));
#else
                            info.icon = QIcon(QtWin::fromHICON(sfi.hIcon));
#endif
                            DestroyIcon(sfi.hIcon);
                        }
                    }
                }
                else
                {
                    info.path = "";
                }
                // Detect 32-bit (WoW64) process
                BOOL isWow64 = FALSE;
                if (IsWow64Process(hProcess, &isWow64) && isWow64)
                    info.is32Bit = true;

                CloseHandle(hProcess);

                processes.append(info);
            }

        } while (Process32NextW(snapshot, &pe32));
    }
    
    CloseHandle(snapshot);
#elif defined(__linux__)
    QDir procDir("/proc");
    QStringList entries = procDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QIcon defaultIcon = qApp->style()->standardIcon(QStyle::SP_ComputerIcon);

    for (const QString& entry : entries) {
        bool ok = false;
        uint32_t pid = entry.toUInt(&ok);
        if (!ok || pid == 0) continue;

        // Read process name from /proc/<pid>/comm
        QString commPath = QStringLiteral("/proc/%1/comm").arg(pid);
        QFile commFile(commPath);
        QString procName;
        if (commFile.open(QIODevice::ReadOnly)) {
            procName = QString::fromUtf8(commFile.readAll()).trimmed();
            commFile.close();
        }
        if (procName.isEmpty()) continue;

        // Read exe path from /proc/<pid>/exe symlink
        QString exePath = QStringLiteral("/proc/%1/exe").arg(pid);
        QFileInfo exeInfo(exePath);
        QString resolvedPath;
        if (exeInfo.exists())
            resolvedPath = exeInfo.symLinkTarget();

        // Skip if we can't read the process memory
        QString memPath = QStringLiteral("/proc/%1/mem").arg(pid);
        if (::access(memPath.toUtf8().constData(), R_OK) != 0)
            continue;

        ProcessInfo info;
        info.pid = pid;
        info.name = procName;
        info.path = resolvedPath;
        info.icon = defaultIcon;

        // Detect 32-bit ELF process
        QFile exeFile(exePath);
        if (exeFile.open(QIODevice::ReadOnly)) {
            QByteArray header = exeFile.read(5);
            if (header.size() >= 5 && header[4] == 1) // ELFCLASS32
                info.is32Bit = true;
            exeFile.close();
        }

        processes.append(info);
    }
#else
    // Platform not supported
    QMessageBox::warning(this, "Error", "Process enumeration not supported on this platform.");
#endif
    
    m_allProcesses = processes;
    applyFilter();
}

void ProcessPicker::populateTable(const QList<ProcessInfo>& processes)
{
    ui->processTable->setRowCount(processes.size());
    
    for (int i = 0; i < processes.size(); ++i) {
        const auto& proc = processes[i];
        
        // PID column
        auto* pidItem = new QTableWidgetItem();
        pidItem->setData(Qt::EditRole, (int)proc.pid);
        ui->processTable->setItem(i, 0, pidItem);
        
        // Name column with icon and architecture indicator
        QString displayName = proc.is32Bit
            ? proc.name + QStringLiteral(" (32-bit)")
            : proc.name;
        auto* nameItem = new QTableWidgetItem(displayName);
        if (!proc.icon.isNull()) {
            nameItem->setIcon(proc.icon);
        }
        // Store original name for selectedProcessName()
        nameItem->setData(Qt::UserRole, proc.name);
        ui->processTable->setItem(i, 1, nameItem);
        
        // Path column with tooltip for full path
        auto* pathItem = new QTableWidgetItem(proc.path);
        pathItem->setToolTip(proc.path);  // Show full path on hover
        ui->processTable->setItem(i, 2, pathItem);
    }
}

void ProcessPicker::filterProcesses(const QString& text)
{
    applyFilter();
}

void ProcessPicker::applyFilter()
{
    QString filterText = ui->filterEdit->text().trimmed();
    
    if (filterText.isEmpty()) {
        populateTable(m_allProcesses);
        return;
    }
    
    QList<ProcessInfo> filtered;
    QString lowerFilter = filterText.toLower();
    
    for (const auto& proc : m_allProcesses) {
        // Match by PID, name, or path
        if (QString::number(proc.pid).contains(lowerFilter) ||
            proc.name.toLower().contains(lowerFilter) ||
            proc.path.toLower().contains(lowerFilter)) {
            filtered.append(proc);
        }
    }
    
    populateTable(filtered);
}
