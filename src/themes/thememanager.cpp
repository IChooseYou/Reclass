#include "thememanager.h"
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>

namespace rcx {

ThemeManager& ThemeManager::instance() {
    static ThemeManager s;
    return s;
}

ThemeManager::ThemeManager() {
    m_builtIn.append(Theme::reclassDark());
    m_builtIn.append(Theme::warm());
    loadUserThemes();

    QSettings settings("Reclass", "Reclass");
    QString saved = settings.value("theme", m_builtIn[0].name).toString();
    auto all = themes();
    for (int i = 0; i < all.size(); i++) {
        if (all[i].name == saved) { m_currentIdx = i; break; }
    }
}

QVector<Theme> ThemeManager::themes() const {
    QVector<Theme> all = m_builtIn;
    all.append(m_user);
    return all;
}

const Theme& ThemeManager::current() const {
    if (m_currentIdx < m_builtIn.size())
        return m_builtIn[m_currentIdx];
    int userIdx = m_currentIdx - m_builtIn.size();
    if (userIdx >= 0 && userIdx < m_user.size())
        return m_user[userIdx];
    return m_builtIn[0];
}

void ThemeManager::setCurrent(int index) {
    auto all = themes();
    if (index < 0 || index >= all.size()) return;
    m_currentIdx = index;
    QSettings settings("Reclass", "Reclass");
    settings.setValue("theme", all[index].name);
    emit themeChanged(current());
}

void ThemeManager::addTheme(const Theme& theme) {
    m_user.append(theme);
    saveUserThemes();
}

void ThemeManager::updateTheme(int index, const Theme& theme) {
    if (index < builtInCount()) {
        // Can't overwrite built-in; save as user theme instead
        m_user.append(theme);
    } else {
        int ui = index - builtInCount();
        if (ui >= 0 && ui < m_user.size())
            m_user[ui] = theme;
    }
    saveUserThemes();
    if (index == m_currentIdx)
        emit themeChanged(current());
}

void ThemeManager::removeTheme(int index) {
    if (index < builtInCount()) return;
    int ui = index - builtInCount();
    if (ui < 0 || ui >= m_user.size()) return;
    m_user.remove(ui);
    if (m_currentIdx == index) {
        m_currentIdx = 0;
        emit themeChanged(current());
    } else if (m_currentIdx > index) {
        m_currentIdx--;
    }
    saveUserThemes();
}

QString ThemeManager::themesDir() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + "/themes";
    QDir().mkpath(dir);
    return dir;
}

void ThemeManager::loadUserThemes() {
    m_user.clear();
    QDir dir(themesDir());
    for (const QString& name : dir.entryList({"*.json"}, QDir::Files)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonDocument jdoc = QJsonDocument::fromJson(f.readAll());
        if (jdoc.isObject())
            m_user.append(Theme::fromJson(jdoc.object()));
    }
}

void ThemeManager::saveUserThemes() const {
    QString dir = themesDir();
    // Remove old files
    QDir d(dir);
    for (const QString& name : d.entryList({"*.json"}, QDir::Files))
        d.remove(name);
    // Write current user themes
    for (int i = 0; i < m_user.size(); i++) {
        QString filename = m_user[i].name.toLower().replace(' ', '_') + ".json";
        QFile f(dir + "/" + filename);
        if (!f.open(QIODevice::WriteOnly)) continue;
        f.write(QJsonDocument(m_user[i].toJson()).toJson(QJsonDocument::Indented));
    }
}

QString ThemeManager::themeFilePath(int index) const {
    if (index < builtInCount()) return {};
    int ui = index - builtInCount();
    if (ui < 0 || ui >= m_user.size()) return {};
    QString filename = m_user[ui].name.toLower().replace(' ', '_') + ".json";
    return themesDir() + "/" + filename;
}

void ThemeManager::previewTheme(const Theme& theme) {
    if (!m_previewing) {
        m_savedTheme = current();
        m_previewing = true;
    }
    emit themeChanged(theme);
}

void ThemeManager::revertPreview() {
    if (m_previewing) {
        m_previewing = false;
        emit themeChanged(m_savedTheme);
    }
}

} // namespace rcx
