#pragma once
#include "themes/theme.h"
#include "sourceentry.h"
#include <QFrame>
#include <QFont>
#include <QVector>

class QAction;
class QLineEdit;
class QListView;
class QStringListModel;
class QLabel;
class QToolButton;
class QGridLayout;
class QMenu;

namespace rcx {

// Fixed connection actions above a bounded source list. Search and the full
// provider catalog reuse that list without moving or resizing the popup.
class SourceChooserPopup : public QFrame {
    Q_OBJECT
public:
    explicit SourceChooserPopup(QWidget* parent = nullptr);
    void setFont(const QFont& font);
    void applyTheme(const Theme& theme);
    void setSources(const QVector<SourceEntry>& entries);
    void setLivenessResults(const QVector<bool>& alive);
    void popup(const QPoint& globalPos, int anchorTop = -1);
    void warmUp();
    const Theme& theme() const { return m_theme; }

signals:
    void sourceSelected(int savedIndex);
    void providerSelected(const QString& identifier);
    void removeRequested(int savedIndex);
    void clearRequested();
    // Filled synchronously by the host; the popup does not own plugin objects.
    void populateProviderMenu(const QString& identifier, QMenu* menu);
    void dismissed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    QLabel* m_titleLabel = nullptr;
    QToolButton* m_menuButton = nullptr;
    QToolButton* m_escBtn = nullptr;
    QLineEdit* m_filterEdit = nullptr;
    QAction* m_clearAction = nullptr;
    QWidget* m_providerArea = nullptr;
    QLabel* m_providerTitle = nullptr;
    QToolButton* m_allButton = nullptr;
    QGridLayout* m_providerGrid = nullptr;
    QVector<QToolButton*> m_providerButtons;
    QStringList m_providerKeys;
    QWidget* m_listHeading = nullptr;
    QLabel* m_listTitle = nullptr;
    QToolButton* m_backButton = nullptr;
    QListView* m_listView = nullptr;
    QStringListModel* m_model = nullptr;
    QLabel* m_emptyLabel = nullptr;
    QLabel* m_footerLabel = nullptr;
    Theme m_theme;
    QFont m_font;
    QVector<SourceEntry> m_allEntries;
    QVector<SourceEntry> m_filteredEntries;
    bool m_showAllProviders = false;
    QString m_lastQuery;
    QPoint m_anchor;
    int m_anchorTop = -1;

    void rebuildProviders();
    void applyFilter(const QString& text);
    void fitToScreen();
    void acceptCurrent();
    void acceptIndex(int row);
    void activateProvider(const QString& id);
    void showEntryMenu(int row, const QPoint& at);
    void showManagementMenu();
    void showProviderMenu(const QString& id, const QPoint& at);
    void styleMenu(QMenu* menu);
    void focusList();
    int nextSelectableRow(int from, int direction) const;
};

} // namespace rcx
