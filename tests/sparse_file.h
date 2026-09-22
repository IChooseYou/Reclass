#pragma once
#include <QFile>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <winioctl.h>
#include <io.h>
#endif

// Avoid allocating tens of GiB on the test runner's filesystem.
inline bool resizeSparseTestFile(QFile& file, qint64 size) {
#ifdef Q_OS_WIN
    DWORD returned = 0;
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    if (!DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr))
        return false;
#endif
    return file.resize(size);
}
