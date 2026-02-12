#pragma once
#include "theme.h"
#include <QDialog>
#include <QVector>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>

class QScrollArea;
class QVBoxLayout;
class QComboBox;

namespace rcx {

class ThemeEditor : public QDialog {
    Q_OBJECT
public:
    explicit ThemeEditor(int themeIndex, QWidget* parent = nullptr);
    Theme result() const { return m_theme; }
    int selectedIndex() const { return m_themeIndex; }

private:
    Theme m_theme;
    int   m_themeIndex;

    // ── Swatch row (compact: label + swatch + hex) ──
    struct SwatchEntry {
        const char*    label;
        QColor Theme::*field;
        QPushButton*   swatchBtn = nullptr;
        QLabel*        hexLabel  = nullptr;
    };
    QVector<SwatchEntry> m_swatches;

    // ── Contrast pair row ──
    struct ContrastEntry {
        const char* fgLabel;
        const char* bgLabel;
        QColor Theme::*fgField;
        QColor Theme::*bgField;
        QLabel* ratioLabel = nullptr;
        QLabel* levelLabel = nullptr;
        QPushButton* fixBtn = nullptr;
    };
    QVector<ContrastEntry> m_contrastPairs;

    // ── UI ──
    QComboBox*   m_themeCombo   = nullptr;
    QLineEdit*   m_nameEdit     = nullptr;
    QLabel*      m_fileInfoLabel = nullptr;
    QPushButton* m_previewBtn   = nullptr;
    bool         m_previewing   = false;

    void loadTheme(int index);
    void rebuildSwatches(QVBoxLayout* swatchLayout);
    void updateSwatch(int idx);
    void updateAllContrast();
    void pickColor(int idx);
    void autoFixContrast(int idx);
    void togglePreview();
};

} // namespace rcx
