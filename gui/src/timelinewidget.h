// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// One trace drawn along the cycle axis: a row per kind of event, each showing
// how many fall in each slice of the visible range. Wheel to zoom about the
// pointer, drag to pan, click to put the cursor on an event, double-click to
// show the whole run. What is drawn comes from rwhost::bucketize; this only
// paints it and turns the mouse into a time.

#ifndef REWIND_GUI_TIMELINEWIDGET_H
#define REWIND_GUI_TIMELINEWIDGET_H

#include <QString>
#include <QWidget>

#include <cstddef>

#include "rewind/tracedoc.h"

class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(const QString& name, QWidget* parent = nullptr);

    // The document is not owned and must outlive the widget's use of it.
    void setDoc(const rwhost::TraceDoc* doc);
    const rwhost::TraceDoc* doc() const { return doc_; }
    void setCaption(const QString& caption);

    // The event the cursor is on. Setting it does not Q_EMIT cursorChanged.
    void setCursor(std::size_t event);
    std::size_t cursor() const { return cursor_; }

    // A mark on an event, for the first place two traces differ.
    void setMarker(bool on, std::size_t event = 0);

    // The visible range of cycles, clamped to the run. Does not emit.
    void setRange(quint64 t0, quint64 t1);
    quint64 rangeStart() const { return t0_; }
    quint64 rangeEnd() const { return t1_; }
    quint64 fullEnd() const;

    // Draw on a longer axis than the trace itself needs, so that two traces of
    // different lengths are drawn to the same scale. 0 goes back to the trace's
    // own length.
    void setAxisEnd(quint64 end);

    // Scrolls, keeping the zoom, so that `event` is in view.
    void reveal(std::size_t event);

    // Where the plot is, and the mapping between x and a cycle count.
    int plotLeft() const;
    int plotWidth() const;
    quint64 timeAtX(int x) const;
    int xAtTime(quint64 t) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    // The user moved the cursor, by click or key.
    void cursorChanged(qulonglong event);
    // The user zoomed or panned.
    void rangeChanged(qulonglong t0, qulonglong t1);

protected:
    void paintEvent(QPaintEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void clampRange();
    void moveCursor(long long delta);

    QString                 name_;
    QString                 caption_;
    const rwhost::TraceDoc* doc_;
    std::size_t             cursor_;
    bool                    marker_on_;
    std::size_t             marker_;
    quint64                 t0_;
    quint64                 t1_;
    quint64                 axis_end_;

    bool    pressed_;
    bool    dragged_;
    QPoint  press_pos_;
    quint64 press_t0_;
    quint64 press_t1_;
};

#endif // REWIND_GUI_TIMELINEWIDGET_H
