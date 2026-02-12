#pragma once
#include "theme.h"
#include <QObject>
#include <QVector>

namespace rcx {

class ThemeManager : public QObject {
    Q_OBJECT
public:
    static ThemeManager& instance();

    QVector<Theme> themes() const;
    int currentIndex() const { return m_currentIdx; }
    const Theme& current() const;

    void setCurrent(int index);
    void addTheme(const Theme& theme);
    void updateTheme(int index, const Theme& theme);
    void removeTheme(int index);

    void loadUserThemes();
    void saveUserThemes() const;

    QString themeFilePath(int index) const;
    void previewTheme(const Theme& theme);
    void revertPreview();

signals:
    void themeChanged(const rcx::Theme& theme);

private:
    ThemeManager();
    QVector<Theme> m_builtIn;
    QVector<Theme> m_user;
    int m_currentIdx = 0;

    int builtInCount() const { return m_builtIn.size(); }
    QString themesDir() const;
    bool m_previewing = false;
    Theme m_savedTheme;   // stashed current theme during preview
};

} // namespace rcx
