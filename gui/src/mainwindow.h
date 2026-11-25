// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The viewer. One trace, or two to compare: a timeline and an event list per
// trace, the firmware's variables at the cursor, and the list of places where
// the two runs part ways. Moving the cursor in one trace moves it to the same
// moment in the other, by the alignment `rewind diff` computes.

#ifndef REWIND_GUI_MAINWINDOW_H
#define REWIND_GUI_MAINWINDOW_H

#include <QMainWindow>
#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

#include "rewind/tracediff.h"
#include "rewind/tracedoc.h"

class EventModel;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QTableView;
class QTabWidget;
class StatePanel;
class TimelineWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    enum Side { kSideA = 0, kSideB = 1 };

    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Main-loop iterations of the recording, as for `rewind --iters`. Set it
    // before opening; it applies to traces opened afterwards.
    void setMainIters(unsigned iters) { main_iters_ = iters; }

    // False, with `error` set, when the trace cannot be read; the side then
    // keeps what it had.
    bool openFile(Side side, const QString& path, QString* error);
    bool openBytes(Side side, const std::vector<rwd::u8>& bytes, const QString& name,
                   QString* error);

    bool hasTrace(Side side) const { return doc_[side] != nullptr; }
    bool comparing() const { return doc_[kSideA] && doc_[kSideB]; }
    // Not const: asking a trace for the firmware's state builds its index the
    // first time.
    rwhost::TraceDoc* doc(Side side) const { return doc_[side].get(); }
    const rwhost::TraceDiff& diff() const { return diff_; }

    std::size_t cursor(Side side) const { return cursor_[side]; }
    // Puts the cursor on an event; the other trace follows to the same moment.
    void setCursor(Side side, std::size_t event);

    void goFirstDifference();
    void goNextDifference();
    void goPreviousDifference();

    TimelineWidget* timeline(Side side) const { return timeline_[side]; }
    QTableView*     table(Side side) const { return table_[side]; }
    StatePanel*     statePanel() const { return state_; }
    QListWidget*    differences() const { return differences_; }
    QString         summary() const;

private Q_SLOTS:
    void onOpen(int side);
    void onDifferenceActivated(QListWidgetItem* item);

private:
    void rebuild();                 // after a trace is opened
    void refresh();                 // after the cursor moves
    void jumpToRegion(std::size_t region);
    QString captionFor(Side side) const;

    // A difference: a run of events that do not match, with the same meaning in
    // both traces -- consecutive only-in-A and only-in-B hunks are one place
    // where the runs parted, not two.
    struct Region {
        std::size_t a, na, b, nb;
    };

    unsigned                           main_iters_;
    std::unique_ptr<rwhost::TraceDoc>  doc_[2];
    QString                            name_[2];
    rwhost::TraceDiff                  diff_;
    std::vector<Region>                regions_;       // list row -> difference
    std::size_t                        cursor_[2];
    bool                               syncing_;

    TimelineWidget* timeline_[2];
    EventModel*     model_[2];
    QTableView*     table_[2];
    StatePanel*     state_;
    QListWidget*    differences_;
    QTabWidget*     tabs_;
    QLabel*         status_;
};

#endif // REWIND_GUI_MAINWINDOW_H
