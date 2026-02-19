#include "thememanager.h"
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QCoreApplication>

namespace rcx {

ThemeManager& ThemeManager::instance() {
    static ThemeManager s;
    return s;
}

ThemeManager::ThemeManager() {
    loadBuiltInThemes();
    loadUserThemes();

    QSettings settings("Reclass", "Reclass");
    QString fallback;
    for (const auto& t : m_builtIn) {
        if (t.name.contains("VS2022", Qt::CaseInsensitive)) { fallback = t.name; break; }
    }
    if (fallback.isEmpty() && !m_builtIn.isEmpty()) fallback = m_builtIn[0].name;
    QString saved = settings.value("theme", fallback).toString();
    auto all = themes();
    for (int i = 0; i < all.size(); i++) {
        if (all[i].name == saved) { m_currentIdx = i; break; }
    }
}

// ── Load built-in themes from JSON files next to the executable ──

QString ThemeManager::builtInDir() const {
    return QCoreApplication::applicationDirPath() + "/themes";
}

void ThemeManager::loadBuiltInThemes() {
    m_builtIn.clear();
    QDir dir(builtInDir());
    if (!dir.exists()) return;
    for (const QString& name : dir.entryList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonDocument jdoc = QJsonDocument::fromJson(f.readAll());
        if (jdoc.isObject())
            m_builtIn.append(Theme::fromJson(jdoc.object()));
    }
    m_builtInDefaults = m_builtIn;
}

// ── themes / current ──

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
    if (!m_builtIn.isEmpty())
        return m_builtIn[0];
    static const Theme empty;
    return empty;
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
    m_previewing = false;  // commit any active preview

    if (index < builtInCount()) {
        m_builtIn[index] = theme;
        m_currentIdx = index;
    } else {
        int ui = index - builtInCount();
        if (ui >= 0 && ui < m_user.size())
            m_user[ui] = theme;
    }
    saveUserThemes();
    QSettings settings("Reclass", "Reclass");
    settings.setValue("theme", current().name);
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

// ── User theme persistence ──

QString ThemeManager::userDir() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + "/themes";
    QDir().mkpath(dir);
    return dir;
}

void ThemeManager::loadUserThemes() {
    m_user.clear();
    QDir dir(userDir());
    for (const QString& name : dir.entryList({"*.json"}, QDir::Files)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        QJsonDocument jdoc = QJsonDocument::fromJson(f.readAll());
        if (!jdoc.isObject()) continue;
        Theme t = Theme::fromJson(jdoc.object());

        // If this overrides a built-in (same name), replace it in-place
        bool isOverride = false;
        for (int i = 0; i < m_builtIn.size(); i++) {
            if (m_builtIn[i].name == t.name) {
                m_builtIn[i] = t;
                isOverride = true;
                break;
            }
        }
        if (!isOverride)
            m_user.append(t);
    }
}

void ThemeManager::saveUserThemes() const {
    QString dir = userDir();
    QDir d(dir);
    for (const QString& name : d.entryList({"*.json"}, QDir::Files))
        d.remove(name);

    // Save modified built-ins (compare against on-disk originals)
    for (int i = 0; i < m_builtIn.size() && i < m_builtInDefaults.size(); i++) {
        if (m_builtIn[i].toJson() != m_builtInDefaults[i].toJson()) {
            QString filename = m_builtIn[i].name.toLower().replace(' ', '_') + ".json";
            QFile f(dir + "/" + filename);
            if (f.open(QIODevice::WriteOnly))
                f.write(QJsonDocument(m_builtIn[i].toJson()).toJson(QJsonDocument::Indented));
        }
    }

    // Save user themes
    for (int i = 0; i < m_user.size(); i++) {
        QString filename = m_user[i].name.toLower().replace(' ', '_') + ".json";
        QFile f(dir + "/" + filename);
        if (f.open(QIODevice::WriteOnly))
            f.write(QJsonDocument(m_user[i].toJson()).toJson(QJsonDocument::Indented));
    }
}

QString ThemeManager::themeFilePath(int index) const {
    if (index < builtInCount()) {
        // Built-in has a user override file only if modified
        if (index < m_builtInDefaults.size()
            && m_builtIn[index].toJson() != m_builtInDefaults[index].toJson()) {
            QString filename = m_builtIn[index].name.toLower().replace(' ', '_') + ".json";
            return userDir() + "/" + filename;
        }
        // Show the built-in source file
        QString filename = m_builtIn[index].name.toLower().replace(' ', '_') + ".json";
        return builtInDir() + "/" + filename;
    }
    int ui = index - builtInCount();
    if (ui < 0 || ui >= m_user.size()) return {};
    QString filename = m_user[ui].name.toLower().replace(' ', '_') + ".json";
    return userDir() + "/" + filename;
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
