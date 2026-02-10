#pragma once
#include <QFrame>
#include <QFont>
#include <QVector>
#include <QString>
#include <cstdint>

class QLineEdit;
class QListView;
class QStringListModel;
class QLabel;
class QToolButton;

namespace rcx {

struct TypeEntry {
    uint64_t id            = 0;
    QString  displayName;
    QString  classKeyword;   // "struct", "class", or "enum"
};

class TypeSelectorPopup : public QFrame {
    Q_OBJECT
public:
    explicit TypeSelectorPopup(QWidget* parent = nullptr);

    void setFont(const QFont& font);
    void setTypes(const QVector<TypeEntry>& types, uint64_t currentId);
    void popup(const QPoint& globalPos);

signals:
    void typeSelected(uint64_t structId);
    void createNewTypeRequested();
    void dismissed();

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QLabel*           m_titleLabel   = nullptr;
    QToolButton*      m_escLabel     = nullptr;
    QToolButton*      m_createBtn    = nullptr;
    QLineEdit*        m_filterEdit   = nullptr;
    QListView*        m_listView     = nullptr;
    QStringListModel* m_model        = nullptr;

    QVector<TypeEntry> m_allTypes;
    QVector<TypeEntry> m_filteredTypes;
    uint64_t           m_currentId = 0;
    QFont              m_font;

    void applyFilter(const QString& text);
    void acceptCurrent();
    void acceptIndex(int row);
};

} // namespace rcx
