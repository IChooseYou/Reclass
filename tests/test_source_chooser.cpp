// Source chooser interaction and rendered geometry. Popups are driven
// synchronously so the hidden Windows test desktop does not dismiss them
// between show() and input. Pixel checks retain the shared popup frame rule.
#include <QtTest/QTest>
#include <QSignalSpy>
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QToolButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScopeGuard>
#include <QScrollBar>
#include <QtMath>
#include "sourcechooserpopup.h"
#include "paintutil.h"
#include "themes/theme.h"
#include "themes/thememanager.h"
using namespace rcx;

namespace {

// A shipped theme by file name, so the chrome rule is checked under both
// polarities regardless of what ThemeManager holds in this bare target.
Theme loadTheme(const QString& baseName) {
    QFile f(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

bool sameColour(QRgb px, const QColor& c, int tol = 2) {
    return qAbs(qRed(px) - c.red()) <= tol && qAbs(qGreen(px) - c.green()) <= tol
        && qAbs(qBlue(px) - c.blue()) <= tol;
}

// Number of pixels in `r` (device) within `tol` of `c`.
int countColour(const QImage& img, const QRect& r, const QColor& c, int tol = 2) {
    int n = 0;
    for (int y = qMax(0, r.top()); y <= r.bottom() && y < img.height(); ++y)
        for (int x = qMax(0, r.left()); x <= r.right() && x < img.width(); ++x)
            if (sameColour(img.pixel(x, y), c, tol)) ++n;
    return n;
}

// The popup rendered onto an image at `dpr` — the painter's device
// transform then carries the scale, so paintutil's edge fills pick device
// rows exactly as they do on a 125 % screen.
QImage renderAt(QWidget& w, qreal dpr) {
    QImage img(qRound(w.width() * dpr), qRound(w.height() * dpr), QImage::Format_ARGB32);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::black);
    QPainter p(&img);
    w.render(&p);
    return img;
}

// The device rows / columns the four edge fills pick for a logical rect at
// `dpr`: qFloor(edge ± 0.5), paintutil's rule.
struct Edges { int top, bottom, left, right; };
Edges edgesOf(const QRect& logical, qreal dpr) {
    const QRectF dev(logical.left() * dpr, logical.top() * dpr,
                     logical.width() * dpr, logical.height() * dpr);
    return { qFloor(dev.top() + 0.5), qFloor(dev.bottom() - 0.5),
             qFloor(dev.left() + 0.5), qFloor(dev.right() - 0.5) };
}
QRect rowAcross(const Edges& e, int y) { return QRect(e.left, y, e.right - e.left + 1, 1); }
QRect colAlong(const Edges& e, int x)  { return QRect(x, e.top, 1, e.bottom - e.top + 1); }

}  // namespace


class TestSourceChooser : public QObject {
    Q_OBJECT
    SourceChooserPopup* m_popup = nullptr;
    QListView* m_list = nullptr;
    QLineEdit* m_search = nullptr;

    static SourceEntry saved(const QString& name, int index, bool active = false) {
        SourceEntry e;
        e.displayName = name;
        e.providerIdentifier = QStringLiteral("processmemory");
        e.kindLabel = kindLabelFor(e.providerIdentifier);
        e.iconPath = iconForProvider(e.providerIdentifier);
        e.pid = QString::number(1000 + index);
        e.providerTarget = e.pid + ':' + name;
        e.savedIndex = index;
        e.isActive = active;
        return e;
    }
    static SourceEntry provider(const QString& name, const QString& id) {
        SourceEntry e;
        e.entryKind = SourceEntry::ProviderAction;
        e.displayName = name;
        e.providerIdentifier = id;
        e.kindLabel = kindLabelFor(id);
        e.iconPath = iconForProvider(id);
        e.dllFileName = QStringLiteral("provider.dll");
        return e;
    }
    static QVector<SourceEntry> liveEntries(int count = 1) {
        QVector<SourceEntry> entries;
        for (int i = 0; i < count; ++i)
            entries.append(saved(QStringLiteral("process_%1.exe").arg(i), i, i == 0));
        entries.append(provider(QStringLiteral("Open File"), QStringLiteral("File")));
        entries.append(provider(QStringLiteral("Kernel Memory"), QStringLiteral("kernelmemory")));
        entries.append(provider(QStringLiteral("Process Memory"), QStringLiteral("processmemory")));
        entries.append(provider(QStringLiteral("ReClass.NET Compat Layer"), QStringLiteral("reclass.netcompatlayer")));
        entries.append(provider(QStringLiteral("Remote Process Memory"), QStringLiteral("remoteprocessmemory")));
        entries.append(provider(QStringLiteral("WinDbg Memory"), QStringLiteral("windbgmemory")));
        return entries;
    }
    static void open(SourceChooserPopup& popup, const QPoint& at) {
        popup.popup(at);
        popup.layout()->activate();
        popup.findChild<QListView*>()->doItemsLayout();
    }
    void reset(const QVector<SourceEntry>& entries) {
        m_popup->hide();
        m_popup->setSources(entries);
        open(*m_popup, QPoint(100, 100));
    }
    int rowNamed(const QString& name) {
        for (int i = 0; i < m_list->model()->rowCount(); ++i)
            if (m_list->model()->index(i, 0).data().toString() == name) return i;
        return -1;
    }
    static void chromeData() {
        QTest::addColumn<QString>("themeName");
        QTest::addColumn<double>("dpr");
        for (const char* theme : {"tw", "vs"})
            for (double scale : {1.0, 1.25, 1.5, 1.75, 2.0})
                QTest::newRow(qPrintable(QStringLiteral("%1@%2").arg(theme).arg(scale)))
                    << QString::fromLatin1(theme) << scale;
    }
private slots:
    void init() {
        m_popup = new SourceChooserPopup;
        m_popup->applyTheme(loadTheme(QStringLiteral("vs")));
        m_popup->setFont(QFont(QStringLiteral("Consolas"), 10));
        m_list = m_popup->findChild<QListView*>();
        m_search = m_popup->findChild<QLineEdit*>();
        reset(liveEntries(2));
    }
    void cleanup() { delete m_popup; }

    void cachedProviderGridDoesNotOverlap_data() {
        QTest::addColumn<int>("providerCount");
        QTest::addColumn<QString>("themeName");
        QTest::addColumn<int>("fontPoints");
        for (int count : {1, 6, 7, 8, 12})
            for (const char* theme : {"vs", "tw"})
                for (int points : {10, 18})
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3pt").arg(count).arg(theme).arg(points)))
                        << count << QString::fromLatin1(theme) << points;
    }

    void cachedProviderGridDoesNotOverlap() {
        QFETCH(int, providerCount);
        QFETCH(QString, themeName);
        QFETCH(int, fontPoints);
        // Exercise the controller's actual lifecycle: create and warm an
        // empty popup, then populate it when the source chip is first clicked.
        m_popup->hide();
        const QString oldStyle = qApp->styleSheet();
        const auto restoreStyle = qScopeGuard([oldStyle] { qApp->setStyleSheet(oldStyle); });
        // The real application has a global style sheet. Its style polish
        // also affects when a newly parented button contributes to size hints.
        qApp->setStyleSheet(QStringLiteral("QScrollBar:vertical { width: 8px; border: none; }"));
        SourceChooserPopup cached;
        cached.warmUp();
        cached.setFont(QFont(QStringLiteral("Consolas"), fontPoints));
        cached.applyTheme(loadTheme(themeName));
        // First open, reuse after search, then a changed provider catalog.
        for (int pass = 0; pass < 3; ++pass) {
            cached.hide();
            const int count = pass == 2 ? 2 : providerCount;
            auto entries = liveEntries(1);
            entries.resize(qMin(count, 6) + 1);
            for (int i = 6; i < count; ++i)
                entries.append(provider(QStringLiteral("Example %1").arg(i), QStringLiteral("example%1").arg(i)));
            cached.setSources(entries);
            cached.popup(QPoint(100, 200));
            const QRect opened = cached.geometry();
            // Child layout requests are deferred. Inspect the settled layout,
            // including on desktops where Qt dismisses native popups on a pump.
            QApplication::processEvents();

            QCOMPARE(cached.geometry(), opened);
            QVERIFY(QApplication::primaryScreen()->availableGeometry().contains(opened));
            auto* area = cached.findChild<QWidget*>(QStringLiteral("providerActions"));
            auto* heading = cached.findChild<QWidget*>(QStringLiteral("sourceListHeading"));
            QVERIFY(cached.rect().contains(area->geometry()));
            QList<QRect> buttons;
            for (auto* button : area->findChildren<QToolButton*>()) {
                if (button->property("providerId").toString().isEmpty()) continue;
                QVERIFY(!button->isHidden());
                const QRect rect(button->mapTo(area, QPoint()), button->size());
                QVERIFY2(area->rect().contains(rect), "Provider button is clipped by its container");
                for (const auto& previous : buttons)
                    QVERIFY2(!previous.intersects(rect), "Provider buttons overlap");
                buttons.append(rect);
            }
            QCOMPARE(buttons.size(), qMin(count, 8));
            QVERIFY(area->geometry().bottom() < heading->geometry().top());
            auto* list = cached.findChild<QListView*>();
            QVERIFY(cached.rect().contains(list->geometry()));
            QVERIFY(list->viewport()->height() >= list->sizeHintForRow(0));
            cached.findChild<QLineEdit*>()->setText(QStringLiteral("process"));
            QApplication::processEvents();
            QCOMPARE(cached.geometry(), opened);
        }
    }

    void fixedActionsAndBoundedHistory() {
        reset(liveEntries(100));
        const QRect before = m_popup->geometry();
        auto* actions = m_popup->findChild<QWidget*>(QStringLiteral("providerActions"));
        const QRect actionRect = actions->geometry();
        QCOMPARE(actions->findChildren<QToolButton*>().size(), 7); // six providers + catalog link
        QCOMPARE(m_list->model()->rowCount(), 100);
        QVERIFY(m_list->verticalScrollBar()->maximum() > 0);
        QVERIFY(m_popup->height() < 520);
        m_list->verticalScrollBar()->setValue(m_list->verticalScrollBar()->maximum());
        QCOMPARE(actions->geometry(), actionRect);
        QCOMPARE(m_popup->geometry(), before);
        for (auto* button : actions->findChildren<QToolButton*>())
            if (!button->property("providerId").toString().isEmpty())
                QVERIFY(!button->isHidden());
    }

    void shortHistoriesOpenCompactAndLargeFontHintsFit() {
        reset(liveEntries(100));
        const int tall = m_popup->height();
        reset(liveEntries(1));
        QVERIFY(m_popup->height() < tall);
        for (int points : {10, 18, 24}) {
            m_popup->setFont(QFont(QStringLiteral("Consolas"), points));
            reset(liveEntries(1));
            auto* footer = m_popup->findChild<QLabel*>(QStringLiteral("sourceFooter"));
            QVERIFY(QFontMetrics(footer->font()).horizontalAdvance(footer->text()) <= footer->contentsRect().width());
            QVERIFY(QApplication::primaryScreen()->availableGeometry().contains(m_popup->geometry()));
        }
    }

    void removingThroughMenuPreservesOpenSearch() {
        auto entries = liveEntries(3);
        reset(entries);
        m_search->setText(QStringLiteral("process_"));
        const QRect geometry = m_popup->geometry();
        connect(m_popup, &SourceChooserPopup::removeRequested, this, [&](int index) {
            entries.removeAt(index);
            for (auto& e : entries) if (e.savedIndex > index) --e.savedIndex;
            m_popup->setSources(entries);
        });
        const int row = rowNamed(QStringLiteral("process_1.exe"));
        const QRect rect = m_list->visualRect(m_list->model()->index(row, 0));
        QTest::mouseClick(m_list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(rect.right() - 16, rect.center().y()));
        auto* menu = m_popup->findChild<QMenu*>(QStringLiteral("sourceRowMenu"));
        QVERIFY(menu);
        QVERIFY(m_popup->isVisible());
        menu->actions().first()->trigger();
        menu->hide();
        QCOMPARE(m_search->text(), QStringLiteral("process_"));
        QCOMPARE(m_popup->geometry(), geometry);
        QCOMPARE(rowNamed(QStringLiteral("process_1.exe")), -1);
        QVERIFY(m_list->currentIndex().isValid());
        QVERIFY(m_popup->isVisible());
    }

    void providerButtonsRouteStableIds() {
        QSignalSpy spy(m_popup, &SourceChooserPopup::providerSelected);
        auto* button = m_popup->findChild<QToolButton*>(QStringLiteral("provider_remoteprocessmemory"));
        QVERIFY(button);
        QVERIFY(button->text().startsWith(QStringLiteral("Process via IPC")));
        QVERIFY(button->toolTip().contains(QStringLiteral("shared memory")));
        button->click();
        QCOMPARE(spy.size(), 1);
        QCOMPARE(spy.first().first().toString(), QStringLiteral("remoteprocessmemory"));
        QVERIFY(!m_popup->isVisible());
    }

    void currentFirstAndClickDoesNotReconnect() {
        auto entries = liveEntries(3);
        entries[0].isActive = false;
        entries[2].isActive = true;
        reset(entries);
        QCOMPARE(m_list->model()->index(0, 0).data().toString(), QStringLiteral("process_2.exe"));
        QSignalSpy selected(m_popup, &SourceChooserPopup::sourceSelected);
        QSignalSpy removed(m_popup, &SourceChooserPopup::removeRequested);
        QTest::mouseClick(m_list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          m_list->visualRect(m_list->model()->index(0, 0)).center());
        QCOMPARE(selected.size(), 0);
        QCOMPARE(removed.size(), 0);
        QVERIFY(!m_popup->isVisible());
    }

    void bodyClickSwitchesWithoutDeleting() {
        QSignalSpy selected(m_popup, &SourceChooserPopup::sourceSelected);
        QSignalSpy removed(m_popup, &SourceChooserPopup::removeRequested);
        QTest::mouseClick(m_list->viewport(), Qt::LeftButton, Qt::NoModifier,
                          m_list->visualRect(m_list->model()->index(1, 0)).center());
        QCOMPARE(selected.size(), 1);
        QCOMPARE(selected.first().first().toInt(), 1);
        QCOMPARE(removed.size(), 0);
    }

    void rowMenuSeparatesRemovalFromSelection() {
        QSignalSpy selected(m_popup, &SourceChooserPopup::sourceSelected);
        QSignalSpy removed(m_popup, &SourceChooserPopup::removeRequested);
        const QRect row = m_list->visualRect(m_list->model()->index(1, 0));
        QTest::mouseClick(m_list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(row.right() - 16, row.center().y()));
        QCOMPARE(selected.size(), 0);
        QCOMPARE(removed.size(), 0);
        auto* menu = m_popup->findChild<QMenu*>(QStringLiteral("sourceRowMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Remove from list"));
        menu->actions().first()->trigger();
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.first().first().toInt(), 1);
        menu->hide();
    }

    void activeRemovalIsExplicitAndKeyboardAccessible() {
        m_list->setCurrentIndex(m_list->model()->index(0, 0));
        QTest::keyClick(m_list, Qt::Key_F10, Qt::ShiftModifier);
        auto* menu = m_popup->findChild<QMenu*>(QStringLiteral("sourceRowMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Disconnect and remove"));
        menu->hide();
    }

    void searchFindsPidLongPathAndOldProviderName() {
        auto entries = liveEntries(2);
        SourceEntry file = saved(QStringLiteral("capture.bin"), 2);
        file.providerIdentifier = QStringLiteral("File");
        file.filePath = QStringLiteral("C:/") + QString(320, 'a') + QStringLiteral("/unique-capture.bin");
        file.pid.clear();
        entries.append(file);
        reset(entries);
        const QRect geometry = m_popup->geometry();
        m_search->setText(QStringLiteral("1001"));
        QVERIFY(rowNamed(QStringLiteral("process_1.exe")) >= 0);
        m_search->setText(QStringLiteral("unique-capture"));
        QVERIFY(rowNamed(QStringLiteral("capture.bin")) >= 0);
        m_search->setText(QStringLiteral("Remote Process Memory"));
        const int row = rowNamed(QStringLiteral("Process via IPC"));
        QVERIFY(row >= 0);
        QSignalSpy picked(m_popup, &SourceChooserPopup::providerSelected);
        QTest::keyClick(m_search, Qt::Key_Return);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.first().first().toString(), QStringLiteral("remoteprocessmemory"));
        QCOMPARE(m_popup->geometry(), geometry);
    }

    void searchExcludesManagementAndCountsOnlyResults() {
        m_search->setText(QStringLiteral("clear"));
        QCOMPARE(m_list->model()->rowCount(), 0);
        QVERIFY(!m_popup->findChild<QLabel*>(QStringLiteral("emptySources"))->isHidden());
        QVERIFY(m_popup->findChild<QLabel*>(QStringLiteral("sourceFooter"))->text().contains(QStringLiteral("Esc")));
        m_search->setText(QStringLiteral("File"));
        QVERIFY(rowNamed(QStringLiteral("File")) >= 0);
        auto* heading = m_popup->findChild<QWidget*>(QStringLiteral("sourceListHeading"));
        QCOMPARE(heading->findChild<QLabel*>()->text(), QStringLiteral("Results (1)"));
    }

    void deletionPreservesQueryIdentityScrollAndGeometry() {
        auto entries = liveEntries(30);
        reset(entries);
        m_search->setText(QStringLiteral("process_"));
        const int selected = rowNamed(QStringLiteral("process_15.exe"));
        m_list->setCurrentIndex(m_list->model()->index(selected, 0));
        m_list->verticalScrollBar()->setValue(140);
        const int scroll = m_list->verticalScrollBar()->value();
        const QRect geometry = m_popup->geometry();
        // Delete an earlier entry, renumbering the controller indices.
        entries.removeAt(1);
        for (auto& e : entries) if (e.savedIndex > 1) --e.savedIndex;
        m_popup->setSources(entries);
        QCOMPARE(m_search->text(), QStringLiteral("process_"));
        QCOMPARE(m_list->currentIndex().data().toString(), QStringLiteral("process_15.exe"));
        QCOMPARE(m_list->verticalScrollBar()->value(), scroll);
        QCOMPARE(m_popup->geometry(), geometry);
        QSignalSpy removed(m_popup, &SourceChooserPopup::removeRequested);
        QTest::keyClick(m_list, Qt::Key_Delete);
        QCOMPARE(removed.size(), 1);
        QCOMPARE(removed.first().first().toInt(), 14);
    }

    void livenessUpdatesDoNotMoveSelection() {
        reset(liveEntries(30));
        m_search->setText(QStringLiteral("process_"));
        m_list->setCurrentIndex(m_list->model()->index(rowNamed(QStringLiteral("process_15.exe")), 0));
        m_list->verticalScrollBar()->setValue(140);
        const int scroll = m_list->verticalScrollBar()->value();
        const QModelIndex current = m_list->currentIndex();
        QVector<bool> alive(30, true);
        alive[15] = false;
        m_popup->setLivenessResults(alive);
        QCOMPARE(m_list->currentIndex(), current);
        QCOMPARE(m_list->verticalScrollBar()->value(), scroll);
        QVERIFY(current.data(Qt::AccessibleDescriptionRole).toString().contains(QStringLiteral("Exited")));
    }

    void fileStatusIsNotAnExitedProcess() {
        auto e = saved(QStringLiteral("missing.bin"), 0);
        e.providerIdentifier = QStringLiteral("File");
        e.pid.clear();
        reset({e});
        m_popup->setLivenessResults({false});
        const auto details = m_list->model()->index(0, 0).data(Qt::AccessibleDescriptionRole).toString();
        QVERIFY(details.contains(QStringLiteral("Missing file")));
        QVERIFY(!details.contains(QStringLiteral("Exited")));
    }

    void futureProvidersFitAndOverflowRemainsSearchable() {
        auto entries = liveEntries();
        for (int i = 0; i < 6; ++i)
            entries.append(provider(QStringLiteral("Transport %1").arg(i), QStringLiteral("transport%1").arg(i)));
        reset(entries);
        auto* area = m_popup->findChild<QWidget*>(QStringLiteral("providerActions"));
        int buttons = 0;
        for (auto* b : area->findChildren<QToolButton*>())
            if (!b->property("providerId").toString().isEmpty()) ++buttons;
        QCOMPARE(buttons, 8);
        const QRect geometry = m_popup->geometry();
        auto* all = m_popup->findChild<QToolButton*>(QStringLiteral("allSources"));
        QVERIFY(!all->isHidden());
        all->click();
        QCOMPARE(m_list->model()->rowCount(), 12);
        QCOMPARE(m_popup->geometry(), geometry);
        QVERIFY(area->isHidden());
        m_search->setText(QStringLiteral("Transport 5"));
        QVERIFY(rowNamed(QStringLiteral("Transport 5")) >= 0);
        m_popup->findChild<QToolButton*>(QStringLiteral("backToSources"))->click();
        QVERIFY(m_search->text().isEmpty());
        QVERIFY(!area->isHidden());
        QCOMPARE(m_list->model()->rowCount(), 1);
        QCOMPARE(m_popup->geometry(), geometry);
    }

    void providerActionsAreAvailableFromManagement() {
        int invoked = 0;
        connect(m_popup, &SourceChooserPopup::populateProviderMenu, this, [&invoked](const QString& id, QMenu* menu) {
            if (id == QStringLiteral("kernelmemory"))
                QObject::connect(menu->addAction(QStringLiteral("Unload driver")), &QAction::triggered,
                                 menu, [&invoked] { ++invoked; });
        });
        m_popup->findChild<QToolButton*>(QStringLiteral("sourceManagement"))->click();
        auto* menu = m_popup->findChild<QMenu*>(QStringLiteral("sourceManagementMenu"));
        QVERIFY(menu);
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Disconnect and clear sources"));
        QMenu* providerMenu = nullptr;
        for (auto* action : menu->actions()) if (action->menu()) providerMenu = action->menu();
        QVERIFY(providerMenu);
        QCOMPARE(providerMenu->title(), QStringLiteral("Kernel"));
        providerMenu->actions().first()->trigger();
        QCOMPARE(invoked, 1);
        menu->hide();
    }

    void emptyHistoryKeepsActionsAndDisablesClear() {
        reset(liveEntries(0));
        QCOMPARE(m_list->model()->rowCount(), 0);
        QVERIFY(!m_popup->findChild<QLabel*>(QStringLiteral("emptySources"))->isHidden());
        m_popup->findChild<QToolButton*>(QStringLiteral("sourceManagement"))->click();
        auto* menu = m_popup->findChild<QMenu*>(QStringLiteral("sourceManagementMenu"));
        QVERIFY(menu);
        QVERIFY(!menu->actions().first()->isEnabled());
        menu->hide();
    }

    void dismissedManagementMenuIsReleased() {
        m_popup->findChild<QToolButton*>(QStringLiteral("sourceManagement"))->click();
        QPointer<QMenu> menu = m_popup->findChild<QMenu*>(QStringLiteral("sourceManagementMenu"));
        QVERIFY(menu);
        menu->hide();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(menu.isNull());
    }

    void keyboardSkipsHeadingsAndDoesNotDeleteProviders() {
        m_search->setText(QStringLiteral("File"));
        QSignalSpy removed(m_popup, &SourceChooserPopup::removeRequested);
        QTest::keyClick(m_search, Qt::Key_Down);
        QCOMPARE(m_list->currentIndex().data().toString(), QStringLiteral("File"));
        QTest::keyClick(m_list, Qt::Key_Delete);
        QCOMPARE(removed.size(), 0);
        QSignalSpy picked(m_popup, &SourceChooserPopup::providerSelected);
        QTest::keyClick(m_list, Qt::Key_Return);
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.first().first().toString(), QStringLiteral("File"));
    }

    void longNamesCannotPaintUnderMenu() {
        reset(liveEntries(2));
        m_list->clearFocus();
        const QRect row = m_list->visualRect(m_list->model()->index(1, 0));
        const QPoint origin = m_list->viewport()->mapTo(m_popup, QPoint());
        const QRect menuRegion(origin.x() + row.right() - 30, origin.y() + row.top(), 24, row.height());
        const QImage before = renderAt(*m_popup, 1);
        auto entries = liveEntries(2);
        entries[1].displayName = QString(400, QLatin1Char('W'));
        m_popup->setSources(entries);
        const QImage after = renderAt(*m_popup, 1);
        QCOMPARE(after.copy(menuRegion), before.copy(menuRegion));
        QVERIFY(m_list->model()->index(1, 0).data(Qt::ToolTipRole).toString().contains(QString(400, QLatin1Char('W'))));
    }

    void pidParsingUsesProviderFormat() {
        QCOMPARE(sourcePid(QStringLiteral("processmemory"), QStringLiteral("123:game.exe")), QStringLiteral("123"));
        QCOMPARE(sourcePid(QStringLiteral("remoteprocessmemory"), QStringLiteral("rpm:456:game.exe")), QStringLiteral("456"));
        QCOMPARE(sourcePid(QStringLiteral("kernelmemory"), QStringLiteral("km:789:game.exe")), QStringLiteral("789"));
        QCOMPARE(sourcePid(QStringLiteral("reclass.netcompatlayer"), QStringLiteral("C:/plugin.dll|42:game.exe")), QStringLiteral("42"));
        QVERIFY(sourcePid(QStringLiteral("socket"), QStringLiteral("123:9000")).isEmpty());
    }

    void compatProviderUsesRegisteredId() {
        const QString name = QStringLiteral("ReClass.NET Compat Layer");
        // PluginManager derives routing IDs from the plugin's declared name.
        const QString id = name.toLower().remove(' ');
        reset({provider(name, id)});
        auto* button = m_popup->findChild<QToolButton*>(QStringLiteral("provider_") + id);
        QVERIFY(button);
        QCOMPARE(button->text(), QStringLiteral("ReClass.NET\u2026"));
        QCOMPARE(iconForProvider(id), QStringLiteral(":/vsicons/plug.svg"));
        QVERIFY(providerOrder(id) < providerOrder(QStringLiteral("example")));
        QSignalSpy picked(m_popup, &SourceChooserPopup::providerSelected);
        button->click();
        QCOMPARE(picked.size(), 1);
        QCOMPARE(picked.first().first().toString(), id);
    }

    void flippingAndUpdatingKeepPopupAttached() {
        const QRect screen = QApplication::primaryScreen()->availableGeometry();
        m_popup->hide();
        const QPoint anchor(screen.right() - 5, screen.bottom() - 20);
        const int barTop = anchor.y() - 28;
        m_popup->popup(anchor, barTop);
        const QRect geometry = m_popup->geometry();
        QCOMPARE(m_popup->y() + m_popup->height(), barTop);
        QVERIFY(screen.contains(geometry));
        m_popup->setSources(liveEntries(100));
        m_search->setText(QStringLiteral("File"));
        QCOMPARE(m_popup->geometry(), geometry);
    }

    void testFrameIsOneDeviceRowOnEverySide_data() { chromeData(); }
    void testFrameIsOneDeviceRowOnEverySide() {
        // The frame is theme.border on exactly the outermost device row /
        // column of each side, unbroken end to end — the active row's
        // accent bar used to sit on the left column and the scrollbar on
        // the right one — and the row / column just inside carries none of
        // it (a 1-logical-px strip lands on two device rows at 125 %).
        QFETCH(QString, themeName);
        QFETCH(double, dpr);
        const Theme t = loadTheme(themeName);
        QVERIFY(t.border.isValid());
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries());
        open(popup, QPoint(100, 100));
        const QImage img = renderAt(popup, dpr);
        const Edges e = edgesOf(popup.rect(), dpr);
        QCOMPARE(e.top, 0);
        QCOMPARE(e.left, 0);
        QCOMPARE(e.bottom, img.height() - 1);
        QCOMPARE(e.right, img.width() - 1);
        const int w = e.right - e.left + 1, h = e.bottom - e.top + 1;
        QCOMPARE(countColour(img, rowAcross(e, e.top), t.border), w);
        QCOMPARE(countColour(img, rowAcross(e, e.bottom), t.border), w);
        QCOMPARE(countColour(img, colAlong(e, e.left), t.border), h);
        QCOMPARE(countColour(img, colAlong(e, e.right), t.border), h);
        // Exactly one row / column thick: just inside the frame there is none.
        QCOMPARE(countColour(img, QRect(e.left + 1, e.top + 1, w - 2, 1), t.border), 0);
        QCOMPARE(countColour(img, QRect(e.left + 1, e.bottom - 1, w - 2, 1), t.border), 0);
        QCOMPARE(countColour(img, QRect(e.left + 1, e.top + 1, 1, h - 2), t.border), 0);
        QCOMPARE(countColour(img, QRect(e.right - 1, e.top + 1, 1, h - 2), t.border), 0);
    }

    void testBodyIsOneSurface_data() { chromeData(); }
    void testBodyIsOneSurface() {
        // One surface: theme.background is the ground everywhere and
        // backgroundAlt appears nowhere (the filter was a cream strip on a
        // grey body). The filter field is that surface with a single
        // device-exact seam under it — containerBorderColor at rest,
        // borderFocused while it has focus — and nothing on the row above.
        QFETCH(QString, themeName);
        QFETCH(double, dpr);
        const Theme t = loadTheme(themeName);
        SourceChooserPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 11));
        popup.setSources(liveEntries());
        open(popup, QPoint(100, 100));
        const QImage img = renderAt(popup, dpr);
        const QRect all(0, 0, img.width(), img.height());
        QVERIFY2(countColour(img, all, t.background) > all.width() * all.height() / 2,
                 "theme.background is not the dominant surface");
        // The four device columns inside each side border carry no ink
        // (gutter + padding), so they show the ground of every band: the
        // field, the list and the footer must all be theme.background there,
        // with only the seam rows (containerBorderColor) in between — no
        // backgroundAlt strip, no stale tint, no accent bar. (A whole-image
        // backgroundAlt count is the wrong probe: on vs it is two units from
        // the greys anti-aliased text passes through.)
        const Edges pe = edgesOf(popup.rect(), dpr);
        for (const QRect strip : { QRect(pe.left + 1, pe.top + 1, 4, pe.bottom - pe.top - 1),
                                   QRect(pe.right - 4, pe.top + 1, 4, pe.bottom - pe.top - 1) }) {
            const int ground = countColour(img, strip, t.background);
            const int seams  = countColour(img, strip, containerBorderColor(t));
            const int focusSeam = countColour(img, strip, t.borderFocused);
            QVERIFY2(ground + seams + focusSeam == strip.width() * strip.height(),
                     qPrintable(QStringLiteral("edge strip at x=%1: %2 ground + %3 seam of %4")
                                    .arg(strip.left()).arg(ground).arg(seams)
                                    .arg(strip.width() * strip.height())));
            QCOMPARE(countColour(img, strip, t.backgroundAlt, 0), 0);
        }
        auto* field = popup.findChild<QLineEdit*>();
        QVERIFY(field);
        const QRect fieldRect(field->mapTo(&popup, QPoint(0, 0)), field->size());
        const Edges f = edgesOf(fieldRect, dpr);
        const int fw = f.right - f.left + 1;
        const int seamRest    = countColour(img, rowAcross(f, f.bottom), containerBorderColor(t));
        const int seamFocused = countColour(img, rowAcross(f, f.bottom), t.borderFocused);
        QVERIFY2(seamRest == fw || seamFocused == fw,
                 qPrintable(QStringLiteral("field seam: %1 rest / %2 focused of %3")
                                .arg(seamRest).arg(seamFocused).arg(fw)));
        QCOMPARE(countColour(img, rowAcross(f, f.bottom - 1), containerBorderColor(t)), 0);
        QCOMPARE(countColour(img, rowAcross(f, f.bottom - 1), t.borderFocused), 0);
        // The band above the seam is the surface (placeholder ink aside).
        QVERIFY(countColour(img, QRect(f.left, f.top, fw, f.bottom - f.top), t.background)
                > fw * (f.bottom - f.top) / 2);
    }


    void seamsReachFrameAtFractionalScales() {
        for (double dpr : {1.0, 1.25, 1.5, 1.75, 2.0}) {
            reset(liveEntries(100));
            const Theme t = m_popup->theme();
            for (int width : {480, 481, 482, 483}) {
                m_popup->setFixedWidth(width);
                m_popup->layout()->activate();
                const QImage img = renderAt(*m_popup, dpr);
                const Edges edges = edgesOf(m_popup->rect(), dpr);
                for (const char* name : {"sourceListHeading", "sourceFooter"}) {
                    auto* w = m_popup->findChild<QWidget*>(QString::fromLatin1(name));
                    QVERIFY(w);
                    const QRect r(w->mapTo(m_popup, QPoint()), w->size());
                    const Edges child = edgesOf(r, dpr);
                    const int y = QByteArray(name) == "sourceFooter" ? child.top : child.bottom;
                    QCOMPARE(countColour(img, QRect(edges.left + 1, y, edges.right - edges.left - 1, 1),
                                         containerBorderColor(t)), edges.right - edges.left - 1);
                }
            }
        }
    }
};

QTEST_MAIN(TestSourceChooser)
#include "test_source_chooser.moc"
