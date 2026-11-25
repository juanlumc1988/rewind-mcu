// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The viewer, driven the way a person drives it -- mouse, wheel, keys -- on a
// window with no screen (QT_QPA_PLATFORM=offscreen). What the viewer shows is
// computed by the host library, which has its own tests; these check that what
// is computed is what ends up on the screen and that the controls do what they
// say.

#include <QApplication>
#include <QImage>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPixmap>
#include <QSet>
#include <QSignalSpy>
#include <QTableView>
#include <QTest>
#include <QWheelEvent>

#include <cstdlib>
#include <vector>

#include "eventmodel.h"
#include "mainwindow.h"
#include "support/blinker.h"
#include "rewind/lanes.h"
#include "rewind/session.h"
#include "rewind/tracediff.h"
#include "rewind/tracedoc.h"
#include "sim/mcu.h"
#include "statepanel.h"
#include "timelinewidget.h"

namespace {

std::vector<rwd::u8> record(rwd::u64 seed, fw::Variant v) {
    rwhost::RunConfig cfg;
    cfg.seed    = seed;
    cfg.variant = v;
    return rwhost::record_run(cfg).trace;
}

void wheel(QWidget* w, int x, int delta) {
    const QPointF pos(x, 30);
    QWheelEvent e(pos, w->mapToGlobal(pos), QPoint(0, 0), QPoint(0, delta), Qt::NoButton,
                  Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(w, &e);
}

QString cellText(const QTableWidget* t, int row, int col) {
    const QTableWidgetItem* it = t->item(row, col);
    return it ? it->text() : QString();
}

} // namespace

class GuiTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void timelineMapsPixelsToCycles();
    void clickPutsTheCursorOnTheEventUnderIt();
    void wheelZoomsAboutThePointer();
    void dragPansWithoutMovingTheCursor();
    void doubleClickShowsTheWholeRun();
    void arrowKeysStepTheCursor();
    void timelineDrawsEveryLane();

    void aSingleTrace();
    void twoTracesAreCompared();
    void theCursorFollowsTheOtherTrace();
    void differencesCanBeWalked();
    void theEventListDrivesTheCursor();
    void aTraceThatCannotBeReadIsRefused();
    void aTraceWithoutFirmwareHasNoState();
    void aFirmwareThatIsNotTheExampleIsShownThroughItsFactory();
    void theWindowIsNotBlank();
};

// ---------------------------------------------------------------- timeline --

void GuiTest::timelineMapsPixelsToCycles() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));

    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 200);
    t.setDoc(&doc);

    QCOMPARE(t.rangeStart(), (quint64)0);
    QCOMPARE(t.rangeEnd(), (quint64)doc.duration());
    QVERIFY(t.plotLeft() + t.plotWidth() <= t.width());
    QCOMPARE(t.timeAtX(t.plotLeft()), (quint64)0);
    QCOMPARE(t.timeAtX(t.plotLeft() + t.plotWidth()), (quint64)doc.duration());

    // Outside the plot is clamped to its ends, not extrapolated.
    QCOMPARE(t.timeAtX(-50), (quint64)0);
    QCOMPARE(t.timeAtX(t.width() + 50), (quint64)doc.duration());

    // x -> cycle -> x is stable to a pixel, and time never runs backwards.
    quint64 prev = 0;
    for (int x = t.plotLeft(); x <= t.plotLeft() + t.plotWidth(); x += 7) {
        const quint64 c = t.timeAtX(x);
        QVERIFY(c >= prev);
        prev = c;
        QVERIFY(std::abs(t.xAtTime(c) - x) <= 1);
    }
}

void GuiTest::clickPutsTheCursorOnTheEventUnderIt() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));
    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 200);
    t.setDoc(&doc);
    QSignalSpy spy(&t, &TimelineWidget::cursorChanged);

    const int x = t.plotLeft() + t.plotWidth() / 2;
    QTest::mouseClick(&t, Qt::LeftButton, Qt::NoModifier, QPoint(x, 40));
    QCOMPARE(spy.count(), 1);

    const std::size_t ev = (std::size_t)spy.takeFirst().at(0).toULongLong();
    QCOMPARE(t.cursor(), ev);
    const quint64 at = t.timeAtX(x);
    // The first event at or after that cycle, and not one earlier.
    QVERIFY(doc.events()[ev].ts >= at);
    QVERIFY(ev == 0 || doc.events()[ev - 1].ts < at);

    // A click at the far right is the end of the run, not past it: the first
    // event of the last cycle (the run's last two events can share one).
    QTest::mouseClick(&t, Qt::LeftButton, Qt::NoModifier,
                      QPoint(t.plotLeft() + t.plotWidth(), 40));
    QCOMPARE(doc.events()[t.cursor()].ts, (rwd::u64)doc.duration());
    QCOMPARE(t.cursor(), rwhost::first_event_at_or_after(doc.events(), doc.duration()));
    QVERIFY(t.cursor() < doc.events().size());
}

void GuiTest::wheelZoomsAboutThePointer() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));
    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 200);
    t.setDoc(&doc);
    QSignalSpy spy(&t, &TimelineWidget::rangeChanged);

    const int x = t.plotLeft() + t.plotWidth() / 4;
    const quint64 under = t.timeAtX(x);
    const quint64 span0 = t.rangeEnd() - t.rangeStart();

    wheel(&t, x, 120);                      // one notch in
    QCOMPARE(spy.count(), 1);
    const quint64 span1 = t.rangeEnd() - t.rangeStart();
    QVERIFY(span1 < span0);
    QVERIFY(std::abs((double)span1 / (double)span0 - 0.8) < 0.01);
    // The cycle under the pointer stays under it, within a pixel's worth.
    const quint64 slack = span1 / (quint64)t.plotWidth() + 2;
    const quint64 now   = t.timeAtX(x);
    QVERIFY((now > under ? now - under : under - now) <= slack);

    for (int i = 0; i < 40; ++i) {
        wheel(&t, x, 120);                  // as far in as it goes
    }
    QVERIFY(t.rangeEnd() > t.rangeStart());
    QVERIFY(t.rangeEnd() - t.rangeStart() >= 16);
    QVERIFY(t.rangeEnd() - t.rangeStart() < span1);

    for (int i = 0; i < 80; ++i) {
        wheel(&t, x, -120);                 // and back out, to the whole run and no further
    }
    QCOMPARE(t.rangeStart(), (quint64)0);
    QCOMPARE(t.rangeEnd(), (quint64)doc.duration());
}

void GuiTest::dragPansWithoutMovingTheCursor() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));
    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 200);
    t.setDoc(&doc);

    const int mid = t.plotLeft() + t.plotWidth() / 2;
    for (int i = 0; i < 6; ++i) {
        wheel(&t, mid, 120);
    }
    const quint64 span = t.rangeEnd() - t.rangeStart();
    const quint64 start0 = t.rangeStart();
    QVERIFY(span < doc.duration());

    QSignalSpy cursor(&t, &TimelineWidget::cursorChanged);
    QSignalSpy range(&t, &TimelineWidget::rangeChanged);

    // Dragging the picture to the right shows what was to the left.
    QTest::mousePress(&t, Qt::LeftButton, Qt::NoModifier, QPoint(mid, 40));
    QTest::mouseMove(&t, QPoint(mid + 60, 40));
    QTest::mouseRelease(&t, Qt::LeftButton, Qt::NoModifier, QPoint(mid + 60, 40));
    QVERIFY(t.rangeStart() < start0);
    QCOMPARE(t.rangeEnd() - t.rangeStart(), span);
    QVERIFY(range.count() >= 1);
    QCOMPARE(cursor.count(), 0);           // a drag is not a click

    // Far enough, and it stops at the start of the run rather than going past
    // it. How far is "far enough" is the whole run's width in pixels.
    const int whole = (int)((long double)doc.duration() * t.plotWidth() / (long double)span) + 50;
    QTest::mousePress(&t, Qt::LeftButton, Qt::NoModifier, QPoint(mid, 40));
    QTest::mouseMove(&t, QPoint(mid + whole, 40));
    QTest::mouseRelease(&t, Qt::LeftButton, Qt::NoModifier, QPoint(mid + whole, 40));
    QCOMPARE(t.rangeStart(), (quint64)0);
    QCOMPARE(t.rangeEnd() - t.rangeStart(), span);

    // And the same at the other end.
    QTest::mousePress(&t, Qt::LeftButton, Qt::NoModifier, QPoint(mid, 40));
    QTest::mouseMove(&t, QPoint(mid - whole, 40));
    QTest::mouseRelease(&t, Qt::LeftButton, Qt::NoModifier, QPoint(mid - whole, 40));
    QCOMPARE(t.rangeEnd(), (quint64)doc.duration());
    QCOMPARE(t.rangeEnd() - t.rangeStart(), span);
    QCOMPARE(cursor.count(), 0);
}

void GuiTest::doubleClickShowsTheWholeRun() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));
    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 200);
    t.setDoc(&doc);

    for (int i = 0; i < 5; ++i) {
        wheel(&t, t.plotLeft() + 100, 120);
    }
    QVERIFY(t.rangeEnd() - t.rangeStart() < doc.duration());
    QTest::mouseDClick(&t, Qt::LeftButton, Qt::NoModifier, QPoint(t.plotLeft() + 100, 40));
    QCOMPARE(t.rangeStart(), (quint64)0);
    QCOMPARE(t.rangeEnd(), (quint64)doc.duration());
}

void GuiTest::arrowKeysStepTheCursor() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));
    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 200);
    t.show();
    t.setDoc(&doc);
    t.setCursor(100);
    t.setFocus();
    QSignalSpy spy(&t, &TimelineWidget::cursorChanged);

    QTest::keyClick(&t, Qt::Key_Right);
    QCOMPARE(t.cursor(), (std::size_t)101);
    QTest::keyClick(&t, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(t.cursor(), (std::size_t)111);
    QTest::keyClick(&t, Qt::Key_Left);
    QCOMPARE(t.cursor(), (std::size_t)110);
    QCOMPARE(spy.count(), 3);

    QTest::keyClick(&t, Qt::Key_Home);
    QCOMPARE(t.cursor(), (std::size_t)0);
    QCOMPARE(spy.count(), 4);
    QTest::keyClick(&t, Qt::Key_Left);                  // already at the start
    QCOMPARE(t.cursor(), (std::size_t)0);
    QCOMPARE(spy.count(), 4);                           // so nothing is said

    QTest::keyClick(&t, Qt::Key_End);
    QCOMPARE(t.cursor(), doc.events().size() - 1);
    QTest::keyClick(&t, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(t.cursor(), doc.events().size() - 1);      // not past the end
}

void GuiTest::timelineDrawsEveryLane() {
    rwhost::TraceDoc doc;
    QVERIFY(doc.open(record(2, fw::kBuggy)));
    TimelineWidget t(QStringLiteral("A"));
    t.resize(800, 160);
    t.show();
    t.setDoc(&doc);
    QTest::qWait(10);

    const QImage img = t.grab().toImage();
    QVERIFY(!img.isNull());
    QSet<QRgb> colours;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            colours.insert(img.pixel(x, y));
        }
    }
    // Reads, writes and interrupts are all in this trace, each in its colour.
    QVERIFY(colours.contains(qRgb(0x3b, 0x82, 0xc4)));
    QVERIFY(colours.contains(qRgb(0xe0, 0x8a, 0x1e)));
    QVERIFY(colours.contains(qRgb(0xd6, 0x45, 0x3d)));
    // The example firmware has no probe points in the buggy variant and no DMA.
    QVERIFY(!colours.contains(qRgb(0x3f, 0x9d, 0x5a)));
    QVERIFY(!colours.contains(qRgb(0x8e, 0x5b, 0xc8)));
}

// ------------------------------------------------------------------ window --

void GuiTest::aSingleTrace() {
    MainWindow w;
    QVERIFY(!w.hasTrace(MainWindow::kSideA));
    QVERIFY(!w.comparing());

    QString err;
    QVERIFY2(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("a.rwd"), &err),
             qPrintable(err));
    w.show();
    QVERIFY(w.hasTrace(MainWindow::kSideA));
    QVERIFY(!w.comparing());
    QVERIFY(w.timeline(MainWindow::kSideA)->isVisibleTo(&w));
    QVERIFY(!w.timeline(MainWindow::kSideB)->isVisibleTo(&w));
    QVERIFY(!w.table(MainWindow::kSideB)->isVisibleTo(&w));
    QVERIFY(w.statePanel()->isColumnHidden(1));
    QCOMPARE(w.differences()->count(), 0);
    QCOMPARE(w.table(MainWindow::kSideA)->model()->rowCount(),
             (int)w.doc(MainWindow::kSideA)->events().size());

    w.setCursor(MainWindow::kSideA, 2088);
    QCOMPARE(w.cursor(MainWindow::kSideA), (std::size_t)2088);
    QCOMPARE(w.table(MainWindow::kSideA)->currentIndex().row(), 2088);
    QCOMPARE(w.timeline(MainWindow::kSideA)->cursor(), (std::size_t)2088);

    // Past the end is the last event.
    w.setCursor(MainWindow::kSideA, 10000000);
    QCOMPARE(w.cursor(MainWindow::kSideA), w.doc(MainWindow::kSideA)->events().size() - 1);

    // The state shown is the firmware's at that point.
    w.setCursor(MainWindow::kSideA, 2088);
    rwhost::Snapshot s;
    QVERIFY(w.doc(MainWindow::kSideA)->snapshot_at(2088, &s));
    const int row = w.statePanel()->rowOf(QStringLiteral("consumed"));
    QVERIFY(row >= 0);
    QCOMPARE(cellText(w.statePanel(), row, 0), QString::number(s.fw.consumed));
    QVERIFY(w.summary().startsWith(QStringLiteral("event 2088 of ")));
}

void GuiTest::twoTracesAreCompared() {
    MainWindow w;
    QString err;
    QVERIFY2(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("buggy.rwd"), &err),
             qPrintable(err));
    QVERIFY2(w.openBytes(MainWindow::kSideB, record(2, fw::kFixed), QStringLiteral("fixed.rwd"), &err),
             qPrintable(err));
    w.show();

    QVERIFY(w.comparing());
    QVERIFY(w.timeline(MainWindow::kSideB)->isVisibleTo(&w));
    QVERIFY(w.table(MainWindow::kSideB)->isVisibleTo(&w));
    QVERIFY(!w.statePanel()->isColumnHidden(1));

    // The same alignment `rewind diff` prints: the runs agree for 2088 events.
    QVERIFY(!w.diff().identical);
    QCOMPARE(w.diff().first, (std::size_t)2088);
    QVERIFY(w.differences()->count() > 0);
    QVERIFY(w.windowTitle().contains(QStringLiteral("buggy.rwd vs fixed.rwd")));

    // Both timelines are drawn to one scale, so a longer run looks longer.
    QCOMPARE(w.timeline(MainWindow::kSideA)->rangeEnd(), w.timeline(MainWindow::kSideB)->rangeEnd());

    w.goFirstDifference();
    QCOMPARE(w.cursor(MainWindow::kSideA), (std::size_t)2088);
    QCOMPARE(w.cursor(MainWindow::kSideB), (std::size_t)2088);

    // At that point the buggy firmware has lost an update the fixed one has not.
    rwhost::Snapshot sa, sb;
    QVERIFY(w.doc(MainWindow::kSideA)->snapshot_at(2088, &sa));
    QVERIFY(w.doc(MainWindow::kSideB)->snapshot_at(2088, &sb));
    QCOMPARE(sa.fw.pending, 0u);
    QCOMPARE(sb.fw.pending, 1u);
    const int pending = w.statePanel()->rowOf(QStringLiteral("pending"));
    const int consumed = w.statePanel()->rowOf(QStringLiteral("consumed"));
    QVERIFY(pending >= 0 && consumed >= 0);
    QCOMPARE(cellText(w.statePanel(), pending, 0), QStringLiteral("0"));
    QCOMPARE(cellText(w.statePanel(), pending, 1), QStringLiteral("1"));
    QCOMPARE(cellText(w.statePanel(), consumed, 0), QString::number(sa.fw.consumed));
    QCOMPARE(cellText(w.statePanel(), consumed, 1), QString::number(sb.fw.consumed));
    QVERIFY(w.statePanel()->differingRows() > 0);

    // A row is flagged exactly when its two numbers differ. The last row is the
    // state's hash: tinted when it differs, and not counted as a variable.
    const QTableWidget* sp = w.statePanel();
    int flagged = 0;
    for (int r = 0; r < sp->rowCount(); ++r) {
        const bool differs = cellText(sp, r, 0) != cellText(sp, r, 1);
        const bool hot = sp->item(r, 0)->background().style() != Qt::NoBrush;
        QCOMPARE(hot, differs);
        QCOMPARE(sp->item(r, 1)->background().style() != Qt::NoBrush, differs);
        if (r + 1 < sp->rowCount()) {
            flagged += differs ? 1 : 0;
        }
    }
    QCOMPARE(sp->verticalHeaderItem(sp->rowCount() - 1)->text(), QStringLiteral("state hash"));
    QCOMPARE(cellText(sp, sp->rowCount() - 1, 0),
             QStringLiteral("%1").arg((qulonglong)sa.state_hash, 16, 16, QLatin1Char('0')));
    QCOMPARE(flagged, w.statePanel()->differingRows());

    QVERIFY(w.summary().contains(QStringLiteral("agree for 2088 events")));

    // The event lists tint what only one run has.
    const EventModel* ma = qobject_cast<const EventModel*>(w.table(MainWindow::kSideA)->model());
    const EventModel* mb = qobject_cast<const EventModel*>(w.table(MainWindow::kSideB)->model());
    QVERIFY(ma && mb);
    std::size_t tinted_a = 0, tinted_b = 0;
    for (std::size_t i = 0; i < w.doc(MainWindow::kSideA)->events().size(); ++i) {
        tinted_a += ma->markAt(i) == EventModel::kMarkOnlyHere ? 1 : 0;
    }
    for (std::size_t i = 0; i < w.doc(MainWindow::kSideB)->events().size(); ++i) {
        tinted_b += mb->markAt(i) == EventModel::kMarkOnlyHere ? 1 : 0;
    }
    QCOMPARE(tinted_a, w.diff().only_a);
    QCOMPARE(tinted_b, w.diff().only_b);
}

void GuiTest::theCursorFollowsTheOtherTrace() {
    MainWindow w;
    QString err;
    QVERIFY(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("a"), &err));
    QVERIFY(w.openBytes(MainWindow::kSideB, record(2, fw::kFixed), QStringLiteral("b"), &err));
    const std::size_t nb = w.doc(MainWindow::kSideB)->events().size();
    const std::size_t na = w.doc(MainWindow::kSideA)->events().size();

    // Before the first difference the two are the same event.
    w.setCursor(MainWindow::kSideA, 100);
    QCOMPARE(w.cursor(MainWindow::kSideB), (std::size_t)100);

    // After it, the same event sits at a different number -- and it is the same
    // event, whichever way round it is asked.
    rwhost::DiffOptions opt;
    opt.ignore_value_at.push_back(sim::kSystickCnt);
    for (std::size_t a : {(std::size_t)3000, (std::size_t)4500, na - 2}) {
        w.setCursor(MainWindow::kSideA, a);
        const std::size_t b = w.cursor(MainWindow::kSideB);
        QCOMPARE(b, rwhost::map_a_to_b(w.diff(), a, nb));
        QVERIFY(rwhost::events_match(w.doc(MainWindow::kSideA)->events()[a],
                                     w.doc(MainWindow::kSideB)->events()[b], opt));
        w.setCursor(MainWindow::kSideB, b);
        QCOMPARE(w.cursor(MainWindow::kSideA), a);
        QCOMPARE(w.cursor(MainWindow::kSideB), b);
    }

    // Moving the cursor in the list of one trace moves the other trace's view too.
    w.setCursor(MainWindow::kSideB, 3001);
    QCOMPARE(w.table(MainWindow::kSideA)->currentIndex().row(), (int)w.cursor(MainWindow::kSideA));
    QCOMPARE(w.table(MainWindow::kSideB)->currentIndex().row(), 3001);
}

void GuiTest::differencesCanBeWalked() {
    MainWindow w;
    QString err;
    QVERIFY(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("a"), &err));
    QVERIFY(w.openBytes(MainWindow::kSideB, record(2, fw::kFixed), QStringLiteral("b"), &err));
    const int n = w.differences()->count();
    QVERIFY(n >= 2);

    // The first one is where the runs part.
    w.goFirstDifference();
    QCOMPARE(w.cursor(MainWindow::kSideA), w.diff().first);

    // Forward visits each in order and then stops at the last.
    std::vector<std::size_t> seen;
    seen.push_back(w.cursor(MainWindow::kSideA));
    for (int i = 0; i < n + 3; ++i) {
        const std::size_t before = w.cursor(MainWindow::kSideA);
        w.goNextDifference();
        if (w.cursor(MainWindow::kSideA) == before) {
            break;
        }
        seen.push_back(w.cursor(MainWindow::kSideA));
    }
    QCOMPARE((int)seen.size(), n);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        QVERIFY(seen[i] > seen[i - 1]);
    }

    // And back again the same way.
    for (std::size_t i = seen.size() - 1; i > 0; --i) {
        QCOMPARE(w.cursor(MainWindow::kSideA), seen[i]);
        w.goPreviousDifference();
    }
    QCOMPARE(w.cursor(MainWindow::kSideA), seen[0]);
    w.goPreviousDifference();                     // nothing before the first
    QCOMPARE(w.cursor(MainWindow::kSideA), seen[0]);

    // Choosing one from the list goes there.
    QListWidget* list = w.differences();
    QVERIFY(list->count() == n);
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier,
                      list->visualItemRect(list->item(n - 1)).center());
    QCOMPARE(w.cursor(MainWindow::kSideA), seen.back());

    // The two traces' cursors are on the differing stretch, in both.
    QVERIFY(w.cursor(MainWindow::kSideA) < w.doc(MainWindow::kSideA)->events().size());
    QVERIFY(w.cursor(MainWindow::kSideB) < w.doc(MainWindow::kSideB)->events().size());
}

void GuiTest::theEventListDrivesTheCursor() {
    MainWindow w;
    QString err;
    QVERIFY(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("a"), &err));
    QVERIFY(w.openBytes(MainWindow::kSideB, record(2, fw::kFixed), QStringLiteral("b"), &err));
    w.show();

    QTableView* tv = w.table(MainWindow::kSideA);
    tv->setFocus();
    tv->setCurrentIndex(tv->model()->index(500, 0));
    QCOMPARE(w.cursor(MainWindow::kSideA), (std::size_t)500);
    QCOMPARE(w.cursor(MainWindow::kSideB), (std::size_t)500);
    QTest::keyClick(tv, Qt::Key_Down);
    QCOMPARE(w.cursor(MainWindow::kSideA), (std::size_t)501);
    QCOMPARE(w.timeline(MainWindow::kSideA)->cursor(), (std::size_t)501);

    // Clicking a timeline moves the list.
    TimelineWidget* t = w.timeline(MainWindow::kSideB);
    QTest::mouseClick(t, Qt::LeftButton, Qt::NoModifier,
                      QPoint(t->plotLeft() + t->plotWidth() * 3 / 4, 40));
    QCOMPARE(w.table(MainWindow::kSideB)->currentIndex().row(), (int)w.cursor(MainWindow::kSideB));
    QCOMPARE(w.timeline(MainWindow::kSideA)->cursor(), w.cursor(MainWindow::kSideA));
}

void GuiTest::aTraceThatCannotBeReadIsRefused() {
    MainWindow w;
    QString err;
    QVERIFY(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("a"), &err));
    const std::size_t events = w.doc(MainWindow::kSideA)->events().size();

    const std::vector<rwd::u8> junk(100, 0x5A);
    QVERIFY(!w.openBytes(MainWindow::kSideB, junk, QStringLiteral("junk.rwd"), &err));
    QVERIFY(err.startsWith(QStringLiteral("junk.rwd: ")));
    QVERIFY(err.size() > 12);
    QVERIFY(!w.hasTrace(MainWindow::kSideB));
    QVERIFY(!w.comparing());

    // A bad replacement leaves the good trace where it was.
    QVERIFY(!w.openBytes(MainWindow::kSideA, junk, QStringLiteral("junk.rwd"), &err));
    QVERIFY(w.hasTrace(MainWindow::kSideA));
    QCOMPARE(w.doc(MainWindow::kSideA)->events().size(), events);

    QVERIFY(!w.openFile(MainWindow::kSideB, QStringLiteral("/nonexistent/dir/x.rwd"), &err));
    QVERIFY(err.contains(QStringLiteral("cannot open")));
}

void GuiTest::aTraceWithoutFirmwareHasNoState() {
    std::vector<rwd::u8> foreign = record(2, fw::kBuggy);
    rwd::put_u64_le(foreign.data() + 8, 0x1234567890ABCDEFULL);   // a build id nobody knows

    MainWindow w;
    QString err;
    QVERIFY2(w.openBytes(MainWindow::kSideA, foreign, QStringLiteral("f.rwd"), &err), qPrintable(err));
    QVERIFY(!w.doc(MainWindow::kSideA)->has_firmware());
    w.setCursor(MainWindow::kSideA, 1000);
    QCOMPARE(w.cursor(MainWindow::kSideA), (std::size_t)1000);      // the events are still there
    QCOMPARE(w.statePanel()->rowCount(), 1);                        // the variables are not:
    QCOMPARE(cellText(w.statePanel(), 0, 0), QStringLiteral("-"));  // only an empty hash row
    QCOMPARE(w.statePanel()->differingRows(), 0);
}

void GuiTest::aFirmwareThatIsNotTheExampleIsShownThroughItsFactory() {
    const rwhost::FirmwareRecording a = blinker::record(5);
    const rwhost::FirmwareRecording b = blinker::record(9);
    QVERIFY(a.writer_ok && b.writer_ok);

    // Without the factory the window does not know this build: events, no state.
    {
        MainWindow plain;
        plain.setMainIters(blinker::kIters);
        QString err;
        QVERIFY2(plain.openBytes(MainWindow::kSideA, a.trace, QStringLiteral("a.rwd"), &err),
                 qPrintable(err));
        QVERIFY(!plain.doc(MainWindow::kSideA)->has_firmware());
        QCOMPARE(plain.statePanel()->rowCount(), 1);
    }

    MainWindow w;
    w.setMainIters(blinker::kIters);
    w.setFirmwareFactory(&blinker::factory);
    QString err;
    QVERIFY2(w.openBytes(MainWindow::kSideA, a.trace, QStringLiteral("a.rwd"), &err), qPrintable(err));
    QVERIFY2(w.openBytes(MainWindow::kSideB, b.trace, QStringLiteral("b.rwd"), &err), qPrintable(err));
    QVERIFY(w.doc(MainWindow::kSideA)->has_firmware());
    QCOMPARE(QString::fromLatin1(w.doc(MainWindow::kSideA)->variant_name()), QStringLiteral("blinker"));

    const std::size_t last = w.doc(MainWindow::kSideA)->events().size() - 1;
    w.setCursor(MainWindow::kSideA, last);
    rwhost::Snapshot sa;
    QVERIFY(w.doc(MainWindow::kSideA)->snapshot_at(w.cursor(MainWindow::kSideA), &sa));

    // The firmware's own fields, with the array elements gathered into one row
    // and the hash as the last.
    const StatePanel* sp = w.statePanel();
    const int presses = sp->rowOf(QStringLiteral("presses"));
    const int history = sp->rowOf(QStringLiteral("history"));
    QVERIFY(presses >= 0);
    QVERIFY(history >= 0);
    QCOMPARE(sp->rowOf(QStringLiteral("history[0]")), -1);
    QCOMPARE(cellText(sp, presses, 0), QString::number(blinker::decode(sa).presses));
    QCOMPARE(cellText(sp, history, 0).split(QLatin1Char(' ')).size(), 4);
    QCOMPARE(sp->verticalHeaderItem(sp->rowCount() - 1)->text(), QStringLiteral("state hash"));

    // Another seed, so another run: some variable differs, and is tinted.
    QVERIFY(sp->differingRows() > 0);
    QVERIFY(w.summary().contains(QStringLiteral("then differ")));
}

void GuiTest::theWindowIsNotBlank() {
    MainWindow w;
    QString err;
    QVERIFY(w.openBytes(MainWindow::kSideA, record(2, fw::kBuggy), QStringLiteral("buggy.rwd"), &err));
    QVERIFY(w.openBytes(MainWindow::kSideB, record(2, fw::kFixed), QStringLiteral("fixed.rwd"), &err));
    w.resize(1180, 720);
    w.show();
    w.goFirstDifference();
    QTest::qWait(20);

    const QImage img = w.grab().toImage();
    QCOMPARE(img.size(), w.size());
    QSet<QRgb> colours;
    for (int y = 0; y < img.height(); y += 2) {
        for (int x = 0; x < img.width(); x += 2) {
            colours.insert(img.pixel(x, y));
        }
    }
    QVERIFY2(colours.size() > 20, "the window looks empty");
    // The differing variables are tinted somewhere in the state panel.
    const QImage panel = w.statePanel()->grab().toImage();
    int tinted = 0;
    for (int y = 0; y < panel.height(); ++y) {
        for (int x = 0; x < panel.width(); ++x) {
            const QColor c = panel.pixelColor(x, y);
            tinted += (c.red() > c.green() + 25 && c.red() > c.blue() + 25) ? 1 : 0;
        }
    }
    QVERIFY2(tinted > 200, "no differing variable is highlighted");
}

QTEST_MAIN(GuiTest)
#include "gui_test.moc"
