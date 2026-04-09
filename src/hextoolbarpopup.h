#pragma once
#include <QFrame>
#include <QFont>
#include <QByteArray>
#include "core.h"

class QLineEdit;

namespace rcx {

struct HexPopupContext {
    uint64_t nodeId = 0;
    NodeKind currentKind = NodeKind::Hex8;
    QByteArray data;            // raw bytes for this node
    // Adjacent same-parent hex nodes after this one (for join preview)
    struct Adjacent {
        bool     exists = false;
        NodeKind kind   = NodeKind::Hex8;
        QByteArray data;
    };
    QVector<Adjacent> nexts;    // up to 15 adjacent nodes (enough for hex8→hex128)

    // Smart suggestions (populated by controller when pinned)
    bool hasPtr = false;
    QString ptrSymbol;
    bool hasFloat = false;
    float floatVal = 0;
    bool hasString = false;
    QString stringPreview;

    // Multi-select (populated by controller)
    int multiSelectCount = 0;
    int multiSelectBytes = 0;
    bool multiSelectContiguous = false;
    NodeKind multiSelectKind = NodeKind::Hex8;  // common kind of selected nodes
};

class HexToolbarPopup : public QFrame {
    Q_OBJECT
public:
    explicit HexToolbarPopup(QWidget* parent = nullptr);

    void setFont(const QFont& font);
    void setContext(const HexPopupContext& ctx);
    void popup(const QPoint& globalPos);
    bool isPinned() const { return m_pinned; }

signals:
    void sizeSelected(uint64_t nodeId, NodeKind newKind);
    void insertAbove(uint64_t nodeId);
    void insertBelow(uint64_t nodeId);
    void joinSelected();
    void fillToOffset(uint64_t nodeId, int targetOffset);
    void dismissed();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QFont m_font;
    HexPopupContext m_ctx;
    int m_hoveredBtn = -1;
    bool m_pinned = false;
    QPoint m_lastPos;       // remember position for re-show when pinned

    // Clickable regions built during paint
    struct HitRect { QRect rect; int action; bool enabled; NodeKind kind; };
    // action codes: 0=size, 1=pin, 2=suggestion, 3=insertAbove, 4=insertBelow, 5=joinSel, 6=fillGo
    QVector<HitRect> m_hits;

    QLineEdit* m_offsetEdit = nullptr;

    void togglePin();
    QSize computeSize() const;
    int maxJoinableBytes() const;
    QString previewForKind(NodeKind target) const;
    QString infoForKind(NodeKind target) const;
    static QString hexLine(const char* typeName, const QByteArray& bytes);
};

} // namespace rcx
