#pragma once
#include "themes/thememanager.h"
#include <QDialog>
#include <QLineEdit>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileInfo>
#include <QDir>
#include <QSettings>
#include <QCoreApplication>
#include <QPainterPath>

namespace rcx {

// Single-widget start page: everything painted in paintEvent.
// Zero CSS, zero Fusion conflicts, zero child-widget styling issues.

class StartPageWidget : public QDialog {
    Q_OBJECT
public:
    explicit StartPageWidget(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);

        m_search = new QLineEdit(this);
        m_search->setPlaceholderText("Search recent...");
        m_search->setFixedHeight(kSearchBarH);
        m_search->setMaximumWidth(330);
        m_search->addAction(QIcon(":/vsicons/search.svg"), QLineEdit::TrailingPosition);
        connect(m_search, &QLineEdit::textChanged, this, [this]{ buildGroups(); update(); });

        loadEntries();
        buildGroups();
        applyTheme(ThemeManager::instance().current());
    }

    void applyTheme(const Theme& t) {
        m_t = t;
        m_search->setStyleSheet(
            "QLineEdit { background: " + t.background.name() + "; color: " + t.text.name()
            + "; border: 1px solid " + t.border.name()
            + "; padding: 2px 8px; font-size: 13px; }"
            "QLineEdit:focus { border: 1px solid " + t.borderFocused.name() + "; }");
        update();
    }

signals:
    void openProject();
    void newClass();
    void importSource();
    void importXml();
    void importPdb();
    void continueClicked();
    void fileSelected(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const int rpX = width() - kCardPanelW - kRightMargin;
        const int lW  = qMax(100, rpX - kPanelGap - kLeftMargin);

        p.fillRect(rect(), m_t.background);

        // ── Title ──
        int y = kTopMargin;
        QFont titleF = font(); titleF.setPixelSize(30); titleF.setWeight(QFont::Light);
        p.setFont(titleF); p.setPen(m_t.text);
        QFontMetrics titleFm(titleF);
        p.drawText(kLeftMargin, y + titleFm.ascent(), "Reclass");
        y += titleFm.height() + 24;

        // ── Headings (left + right at same y) ──
        QFont headF = font(); headF.setPixelSize(20); headF.setWeight(QFont::DemiBold);
        p.setFont(headF); QFontMetrics headFm(headF);
        p.drawText(kLeftMargin, y + headFm.ascent(), "Open recent");
        int ry = y;
        p.drawText(rpX, ry + headFm.ascent(), "Get started");
        ry += headFm.height() + 14;
        y  += headFm.height() + 14;

        // ── Search bar (only child widget) ──
        m_search->setGeometry(kLeftMargin, y, qMin(330, lW), kSearchBarH);
        y += kSearchBarH + kSearchGap;
        m_listTop = y;

        // ── Right panel ──
        drawCards(p, rpX, ry, kCardPanelW);

        // ── File list ──
        drawFileList(p, kLeftMargin, lW);

        // ── Border ──
        p.setPen(QPen(m_t.border, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        auto [z, i] = hitTest(e->pos());
        if (z != m_hz || i != m_hi) {
            m_hz = z; m_hi = i;
            setCursor(z != HZ_None ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        auto [z, i] = hitTest(e->pos());
        if (z == HZ_Entry)    emit fileSelected(m_filtered[i].path);
        if (z == HZ_Group)    { m_groups[i].expanded = !m_groups[i].expanded; update(); }
        if (z == HZ_Card && i == 0) emit newClass();
        if (z == HZ_Card && i == 1) emit openProject();
        if (z == HZ_Card && i == 2) emit importSource();
        if (z == HZ_Card && i == 3) emit importXml();
        if (z == HZ_Card && i == 4) emit importPdb();
        if (z == HZ_Continue) emit continueClicked();
    }

    void wheelEvent(QWheelEvent* e) override {
        m_scrollY = qBound(0, m_scrollY - e->angleDelta().y() / 2, m_maxScroll);
        update();
    }

    void resizeEvent(QResizeEvent* e) override { QWidget::resizeEvent(e); update(); }
    void leaveEvent(QEvent*) override { m_hz = HZ_None; m_hi = -1; setCursor(Qt::ArrowCursor); update(); }
    void keyPressEvent(QKeyEvent* e) override { if (e->key() == Qt::Key_Escape) reject(); }

private:
    enum HZ { HZ_None, HZ_Entry, HZ_Group, HZ_Card, HZ_Continue };
    struct Hit { HZ zone; int idx; };

    struct Entry {
        QString path, fileName, dirPath;
        QDateTime lastModified;
        bool isExample;
    };
    struct Group {
        QString name;
        bool expanded = true;
        QVector<int> entries;
    };

    // ── Layout constants (single source of truth for paint + hitTest) ──
    static constexpr int kLeftMargin   = 48;   // left inset for title + file list
    static constexpr int kTopMargin    = 36;   // top inset for title
    static constexpr int kRightMargin  = 32;   // right inset for cards panel
    static constexpr int kPanelGap     = 40;   // gap between file list and cards
    static constexpr int kCardPanelW   = 340;  // right-side cards panel width
    static constexpr int kCardH        = 84;   // single card row height
    static constexpr int kEntryH       = 52;   // single file entry row height
    static constexpr int kGroupHeaderH = 28;   // group label row height
    static constexpr int kGroupSpacing = 15;   // vertical gap between groups
    static constexpr int kBottomPad    = 24;   // padding below file list / border inset
    static constexpr int kSearchBarH   = 30;   // search bar fixed height
    static constexpr int kSearchGap    = 16;   // gap below search bar before list

    Theme m_t;
    QLineEdit* m_search;
    QVector<Entry> m_all, m_filtered;
    QVector<Group> m_groups;
    int m_scrollY = 0, m_maxScroll = 0, m_listTop = 0, m_contentH = 0;

    HZ  m_hz = HZ_None;
    int m_hi = -1;

    // Hit rects populated during paint
    QVector<QPair<int, QRectF>> m_grpRects, m_entRects;
    QRectF m_cardR[5], m_contR;

    void drawIcon(QPainter& p, const QString& path, int x, int y, int sz) {
        QIcon(path).paint(&p, x, y, sz, sz);
    }

    // ── Data loading ──

    void loadEntries() {
        m_all.clear();
        QSettings s("Reclass", "Reclass");
        for (const auto& path : s.value("recentFiles").toStringList()) {
            QFileInfo fi(path);
            if (!fi.exists()) continue;
            m_all.push_back(Entry{fi.absoluteFilePath(), fi.fileName(), fi.absolutePath(),
                                 fi.lastModified(), false});
        }
#ifdef __APPLE__
        QDir exDir(QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../Resources/examples"));
#else
        QDir exDir(QCoreApplication::applicationDirPath() + "/examples");
#endif
        for (const auto& fn : exDir.entryList({"*.rcx"}, QDir::Files, QDir::Name))
            m_all.push_back(Entry{exDir.absoluteFilePath(fn), fn, exDir.absolutePath(),
                                 QFileInfo(exDir.filePath(fn)).lastModified(), true});
    }

    void buildGroups() {
        QString f = m_search->text().trimmed().toLower();
        m_filtered.clear();
        for (const auto& e : m_all)
            if (f.isEmpty() || e.fileName.toLower().contains(f) || e.dirPath.toLower().contains(f))
                m_filtered.append(e);

        QDate today = QDate::currentDate();
        QVector<int> bk[6];
        for (int i = 0; i < m_filtered.size(); i++) {
            auto& e = m_filtered[i];
            if (e.isExample) { bk[5].append(i); continue; }
            int d = e.lastModified.date().daysTo(today);
            if      (d == 0) bk[0].append(i);
            else if (d == 1) bk[1].append(i);
            else if (d <  7) bk[2].append(i);
            else if (e.lastModified.date().month() == today.month()
                  && e.lastModified.date().year()  == today.year()) bk[3].append(i);
            else bk[4].append(i);
        }
        static const char* names[] = {"Today","Yesterday","This week","This month","Older","Examples"};
        m_groups.clear();
        for (int i = 0; i < 6; i++)
            if (!bk[i].isEmpty()) m_groups.push_back(Group{names[i], true, bk[i]});
        m_scrollY = 0;
    }

    // ── Drawing ──

    void drawCards(QPainter& p, int x, int y, int w) {
        struct C { const char* icon; const char* title; const char* desc; };
        static const C cards[] = {
            {":/vsicons/symbol-structure.svg", "New Class",          "Start a new binary class definition"},
            {":/vsicons/folder-opened.svg",    "Open project",      "Open an existing .rcx project"},
            {":/vsicons/file-binary.svg",      "Import from Source", "Import C/C++ header or source file"},
            {":/vsicons/code.svg",             "Import ReClass XML", "Import from ReClass .xml format"},
            {":/vsicons/debug.svg",            "Import PDB",         "Import types from a .pdb symbol file"}
        };

        const int N = 5, panelH = N * kCardH;

        // Sharp-cornered panel background
        p.save();
        p.setClipRect(QRectF(x, y, w, panelH));
        p.fillRect(x, y, w, panelH, m_t.background);

        for (int i = 0; i < N; i++) {
            int cy = y + i * kCardH;
            QRectF cr(x, cy, w, kCardH);
            m_cardR[i] = cr;
            bool hov = (m_hz == HZ_Card && m_hi == i);

            if (hov) {
                p.fillRect(cr, m_t.hover);
                p.fillRect(QRectF(x, cy, 3, kCardH), m_t.indHoverSpan);
            }

            // Icon (32px, centered vertically)
            int iconSz = 32;
            drawIcon(p, cards[i].icon, x + 24, cy + (kCardH - iconSz) / 2, iconSz);

            // Title + description block, centered vertically
            int tx = x + 24 + iconSz + 16;
            QFont tf = font(); tf.setPixelSize(15);
            QFont df = font(); df.setPixelSize(12);
            QFontMetrics tfm(tf), dfm(df);
            int blockH = tfm.height() + 5 + dfm.height();
            int by = cy + (kCardH - blockH) / 2;

            p.setFont(tf); p.setPen(m_t.text);
            p.drawText(tx, by + tfm.ascent(), cards[i].title);
            p.setFont(df); p.setPen(m_t.textDim);
            p.drawText(tx, by + tfm.height() + 5 + dfm.ascent(), cards[i].desc);
        }

        p.restore();

        // "Continue →" centered under the panel
        int cy = y + panelH + 8;
        QFont lf = font(); lf.setPixelSize(13);
        if (m_hz == HZ_Continue) lf.setUnderline(true);
        p.setFont(lf); p.setPen(m_t.indHoverSpan);
        QFontMetrics lfm(lf);
        QString ct = QStringLiteral("Tutorial  \u2192");
        int cw = lfm.horizontalAdvance(ct);
        m_contR = QRectF(x + (w - cw) / 2, cy, cw, lfm.height());
        p.drawText(int(m_contR.x()), cy + lfm.ascent(), ct);
    }

    void drawFileList(QPainter& p, int x, int w) {
        int listH = height() - kBottomPad - m_listTop;
        p.save();
        p.setClipRect(x, m_listTop, w, listH);

        int fy = m_listTop - m_scrollY;
        m_grpRects.clear();
        m_entRects.clear();

        for (int gi = 0; gi < m_groups.size(); gi++) {
            auto& g = m_groups[gi];
            if (gi > 0) fy += kGroupSpacing;

            // Group header
            m_grpRects.emplaceBack(gi, QRectF(x, fy, w, kGroupHeaderH));
            p.setPen(Qt::NoPen); p.setBrush(m_t.text);
            int triX = x + 8, triY = fy + 11;
            QPolygonF tri;
            if (g.expanded) tri << QPointF(triX,triY) << QPointF(triX+6,triY) << QPointF(triX+3,triY+6);
            else            tri << QPointF(triX,triY) << QPointF(triX+6,triY+3) << QPointF(triX,triY+6);
            p.drawPolygon(tri);

            QFont gf = font(); gf.setPixelSize(13);
            p.setFont(gf); p.setPen(m_t.text);
            p.drawText(triX + 14, fy + kGroupHeaderH / 2 + QFontMetrics(gf).ascent() / 2 - 1, g.name);
            fy += kGroupHeaderH;

            if (!g.expanded) continue;

            for (int ei : g.entries) {
                auto& e = m_filtered[ei];
                QRectF er(x, fy, w, kEntryH);
                m_entRects.emplaceBack(ei, er);
                if (m_hz == HZ_Entry && m_hi == ei) p.fillRect(er, m_t.hover);

                drawIcon(p, e.isExample ? ":/vsicons/book.svg" : ":/vsicons/symbol-structure.svg",
                         x + 24, fy + 17, 18);

                int tx = x + 52, avail = w - 64;
                QFont nf = font(); nf.setPixelSize(14);
                p.setFont(nf); p.setPen(m_t.text);
                QFontMetrics nm(nf);
                int ny = fy + 8;
                p.drawText(tx, ny + nm.ascent(),
                           nm.elidedText(e.fileName, Qt::ElideMiddle, avail * 0.65));

                if (!e.isExample) {
                    p.setPen(m_t.textDim);
                    QString dt = e.lastModified.toString("M/d/yyyy h:mm AP");
                    p.drawText(x + w - 12 - nm.horizontalAdvance(dt), ny + nm.ascent(), dt);
                }

                QFont pf = font(); pf.setPixelSize(12);
                p.setFont(pf); p.setPen(m_t.textDim);
                QFontMetrics pm(pf);
                p.drawText(tx, ny + nm.height() + 4 + pm.ascent(),
                           pm.elidedText(e.dirPath, Qt::ElideMiddle, avail));
                fy += kEntryH;
            }
        }

        m_contentH = fy + m_scrollY - m_listTop;
        m_maxScroll = qMax(0, m_contentH - listH);
        p.restore();
    }

    // ── Hit testing ──

    Hit hitTest(QPoint pos) const {
        for (int i = 0; i < 5; i++)
            if (m_cardR[i].contains(pos)) return {HZ_Card, i};
        if (m_contR.contains(pos)) return {HZ_Continue, 0};
        if (pos.y() >= m_listTop && pos.y() < height() - kBottomPad) {
            for (const auto& [gi, r] : m_grpRects)
                if (r.contains(pos)) return {HZ_Group, gi};
            for (const auto& [ei, r] : m_entRects)
                if (r.contains(pos)) return {HZ_Entry, ei};
        }
        return {HZ_None, -1};
    }
};

} // namespace rcx
