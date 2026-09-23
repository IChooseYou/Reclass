// Render harness for rcx::TimelineStrip.
//
// Builds the strip standalone — no controller, no timeline — feeds it
// synthetic states and deterministic change generators, and grabs every
// scenario at 240 / 300 / 480 / 760 / 1080 / 1920 logical px, stacked with a
// magenta gap between rows (never black: black would read as an unpainted
// band). Run it on the hidden desktop via tools/run_tests_hidden.py's
// run_hidden(), once plain and once with QT_SCALE_FACTOR=1.25, in both the
// light and the dark theme, and LOOK at every sheet.
//
// Usage: timeline_render <out-prefix> [themeJsonPath|themeName]
//
// Output: <out-prefix>_<w>.png per width, and on stdout, per row, the
// logical rect of every laid-out cell plus the dpr.
//
// The strip is the graph and one overflow button; Record / Stop and Back to
// live are on the address bar. No time is written on the graph.
//
// Rows, top to bottom:
//   0  static source (the message in the graph)
//   1  nothing recorded yet ("Press Record…")
//   2  recording a bouncing ball, 7 s in: steady churn, beads at bounces
//   3  a finished 45 s ball recording (stopped), a bounce every ~1.4 s
//   4  recorded, stopped for a while, recorded again (dotted gap)
//   5  looking back at −60 % of a ball recording (amber playhead + wash)
//   6  a selected field changing (selection lane) and a rebase marker
//   7  bursty: quiet, then bursts of many fields
//   8  one single change in the whole recording

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QTextStream>

#include "themes/theme.h"
#include "widgets/chrome_fallback_theme.h"
#include "widgets/timeline_strip.h"

using namespace rcx;

static bool loadThemeFile(const QString& path, Theme& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    out = Theme::fromJson(doc.object());
    return out.background.isValid();
}

static bool resolveTheme(const QString& arg, Theme& out) {
    if (arg.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) && QFileInfo::exists(arg))
        return loadThemeFile(arg, out);
    QStringList dirs{QCoreApplication::applicationDirPath() + QStringLiteral("/themes")};
#ifdef RCX_SOURCE_DIR
    dirs << QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults");
#endif
    for (const QString& dir : dirs) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QString& name : d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            Theme t;
            if (!loadThemeFile(d.filePath(name), t)) continue;
            if (QFileInfo(name).completeBaseName().compare(arg, Qt::CaseInsensitive) == 0
                || t.name.compare(arg, Qt::CaseInsensitive) == 0) {
                out = t;
                return true;
            }
        }
    }
    return false;
}

namespace {

constexpr int64_t kNow = 10'000'000;   // capture clock "now" for every row
constexpr int64_t kTick = 200;         // a record every refresh tick

// A bouncing ball: every tick ~9 fields move (time, position, velocity,
// transform…); a bounce also changes bounces, colour and squash; a restart
// every ~10 s changes state and restarts too. Returns fields changed, and via
// `started` how many of them were not changing the tick before.
uint32_t ballAt(int64_t t, int* started = nullptr) {
    const int64_t k = t / kTick;
    const int64_t phase = k % 50;                     // 10 s cycle
    const bool resting = phase >= 38;                 // lies still, then restarts
    const bool restart = phase == 0;
    const bool bounce = !resting && phase > 2 && (phase % 7 == 0);
    uint32_t v = resting ? 1 : 9;                     // resting: only the countdown moves
    int s = 0;
    if (bounce) { v += 3; s = 3; }
    if (restart) { v += 4; s = 4; }
    if (phase == 38) s = std::max(s, 1);              // came to rest
    if (started) *started = s;
    return v;
}

uint32_t burstyAt(int64_t t) {
    const int64_t s = t / 1000;
    uint32_t v = 0;
    if (s % 23 == 0) v = 4;
    if (s % 97 < 3) v = std::max<uint32_t>(v, 60 + uint32_t(s % 7) * 20);
    return v;
}

TimelineStripState stateFor(int row) {
    TimelineStripState s;
    s.nowMs = kNow;
    s.epochAtZeroMs = 1'700'000'000'000LL;
    s.dataGeneration = quint64(row + 1);
    s.hasData = true;
    s.capture = timeline::Capture::Rolling;
    switch (row) {
    case 0:
        s = TimelineStripState{};
        s.capture = timeline::Capture::Static;
        s.nowMs = kNow;
        break;
    case 1:
        s.hasData = false;
        s.retainedBeginMs = kNow;
        break;
    case 2:
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = kNow - 7'000;
        s.recordBeginMs = s.retainedBeginMs;
        break;
    case 3:
        s.retainedBeginMs = kNow - 45'000;
        s.gaps.append({kNow, -1});                   // stopped at "now"
        break;
    case 4:
        s.retainedBeginMs = kNow - 60'000;
        s.gaps.append({kNow - 38'000, kNow - 22'000});
        s.gaps.append({kNow, -1});
        break;
    case 5:
        s.retainedBeginMs = kNow - 30'000;
        s.past = true;
        s.viewedMs = kNow - 18'000;
        break;
    case 6:
        s.retainedBeginMs = kNow - 30'000;
        s.selectionKey = 7;
        s.markers.append({kNow - 12'000, TimelineMarkerKind::Rebase, QStringLiteral("0x7FF6DEAD1234")});
        break;
    case 7:
        s.retainedBeginMs = kNow - 300'000;
        break;
    case 8:
        s.retainedBeginMs = kNow - 60'000;
        break;
    // 9, 10: fed by records rather than binned columns — the renderer that
    // actually ships. 9 recording and scrolling, 10 with a quiet stretch.
    case 9:
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = kNow - 60'000;
        s.recordBeginMs = s.retainedBeginMs;
        break;
    case 10:
        s.capture = timeline::Capture::Recording;
        s.retainedBeginMs = kNow - 60'000;
        s.recordBeginMs = s.retainedBeginMs;
        s.gaps.append({kNow - 34'000, kNow - 31'000});
        break;
    }
    return s;
}

TimelineStrip::Callbacks callbacksFor(int row) {
    TimelineStrip::Callbacks cb;
    const bool bursty = row == 7;
    const bool single = row == 8;
    cb.columns = [bursty, single](int64_t t0, int64_t step, int n, QVector<TimelineColumn>& out) {
        out.resize(n);
        for (int i = 0; i < n; ++i) {
            const int64_t a = t0 + step * i, b = a + step;
            uint32_t mx = 0;
            int records = 0;
            for (int64_t t = ((a + kTick - 1) / kTick) * kTick; t < b; t += kTick) {
                uint32_t v = 0;
                if (single) v = (t == kNow - 25'000) ? 4 : 0;
                else if (bursty) v = burstyAt(t);
                else v = ballAt(t);
                if (v == 0) continue;
                mx = std::max(mx, v);
                ++records;
                if (records > 64) break;
            }
            out[i].maxChanged = mx;
            out[i].records = records;
        }
    };
    if (!bursty && !single) {
        cb.events = [](int64_t t0, int64_t step, int n, QVector<uint32_t>& out) {
            out.fill(0, n);
            for (int i = 0; i < n; ++i) {
                const int64_t a = t0 + step * i;
                for (int64_t t = ((a + kTick - 1) / kTick) * kTick; t < a + step; t += kTick) {
                    int started = 0;
                    ballAt(t, &started);
                    out[i] = std::max(out[i], uint32_t(started));
                }
            }
        };
    }
    if (row == 6) {
        cb.selection = [](int64_t t0, int64_t step, int n, QVector<char>& out) {
            out.fill(0, n);
            for (int i = 0; i < n; ++i) {
                const int64_t a = t0 + step * i;
                for (int64_t t = ((a + kTick - 1) / kTick) * kTick; t < a + step; t += kTick) {
                    int started = 0;
                    ballAt(t, &started);
                    if (started == 3) out[i] = 1;          // "bounces" is selected
                }
            }
        };
    }
    if (row == 9 || row == 10) {
        const int64_t quietFrom = row == 10 ? kNow - 34'000 : 0;
        const int64_t quietTo = row == 10 ? kNow - 31'000 : 0;
        cb.points = [quietFrom, quietTo](int64_t t0, int64_t t1, int cap,
                                         QVector<TimelinePoint>& out) {
            out.clear();
            for (int64_t t = ((t0 + kTick - 1) / kTick) * kTick; t < t1; t += kTick) {
                if (out.size() >= cap) return false;
                if (t > kNow) break;
                if (quietTo > quietFrom && t >= quietFrom && t < quietTo) continue;
                int started = 0;
                const uint32_t v = ballAt(t, &started);
                if (v == 0) continue;
                out.append(TimelinePoint{t, v, uint32_t(started), false});
            }
            return true;
        };
    }
    cb.describe = [](int64_t t) {
        const uint32_t v = ballAt(t);
        return QStringLiteral("%1 fields changed: time, position, velocity").arg(v);
    };
    return cb;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    if (argc < 2) {
        QTextStream(stderr) << "usage: timeline_render <out-prefix> [themeJsonPath|themeName]\n";
        return 2;
    }
    const QString prefix = QString::fromLocal8Bit(argv[1]);
    Theme theme = chromeFallbackTheme();
    if (argc >= 3 && !resolveTheme(QString::fromLocal8Bit(argv[2]), theme)) {
        QTextStream(stderr) << "theme not found: " << argv[2] << "\n";
        return 2;
    }

    QTextStream out(stdout);
    const int widths[] = {240, 300, 480, 760, 1080, 1920};
    constexpr int kRows = 11;   // 9, 10: the record-fed trace (what actually ships)
    constexpr int kGap = 4;
    for (int w : widths) {
        QVector<QPixmap> grabs;
        qreal dpr = 1.0;
        for (int row = 0; row < kRows; ++row) {
            QWidget host;
            host.resize(w, TimelineStrip::kHeight);
            auto* strip = new TimelineStrip(&host);
            strip->setGeometry(0, 0, w, TimelineStrip::kHeight);
            strip->applyTheme(theme);
            // The strip advances "now" on its own clock between pushes; hold
            // it still so every sheet is byte-identical run to run.
            strip->setClockForTest([] { return int64_t(0); });
            strip->setCallbacks(callbacksFor(row));
            strip->setState(stateFor(row));
            host.show();
            QCoreApplication::processEvents();
            QPixmap pm = strip->grab();
            dpr = pm.devicePixelRatio();
            grabs.append(pm);
            out << "width " << w << " row " << row << " dpr " << dpr << "\n";
            for (const auto& c : strip->layout().cells)
                out << "  " << c.id << " " << c.rect.x() << "," << c.rect.y() << " "
                    << c.rect.width() << "x" << c.rect.height() << "\n";
            const QRect g = strip->graphRect();
            out << "  graph " << g.x() << "," << g.y() << " " << g.width() << "x" << g.height()
                << "  step " << strip->window().stepMs << "ms"
                << "  playhead " << strip->playheadX() << "\n";
        }
        const int devW = int(std::lround(w * dpr));
        const int devRowH = grabs.isEmpty() ? 0 : grabs.first().height();
        QImage sheet(devW, kRows * devRowH + (kRows - 1) * kGap, QImage::Format_RGB32);
        sheet.fill(QColor(255, 0, 255));
        QPainter p(&sheet);
        for (int row = 0; row < grabs.size(); ++row) {
            QPixmap pm = grabs[row];
            pm.setDevicePixelRatio(1.0);
            p.drawPixmap(0, row * (devRowH + kGap), pm);
        }
        p.end();
        const QString path = QStringLiteral("%1_%2.png").arg(prefix).arg(w);
        sheet.save(path);
        out << "wrote " << path << "\n";
    }
    out.flush();
    return 0;
}
