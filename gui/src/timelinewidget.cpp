// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "timelinewidget.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "rewind/lanes.h"

namespace {

const int kLabelWidth = 96;
const int kRightMargin = 8;
const int kTitleHeight = 22;
const int kRulerHeight = 18;
const int kRowHeight = 17;
const int kBottomMargin = 6;
const int kPxPerBucket = 3;
const quint64 kMinSpan = 16;   // cycles; zooming in further shows nothing new

// One colour per lane, chosen to stay apart on a light and on a dark background.
QColor laneColor(int lane) {
    static const QColor c[rwhost::kLaneCount] = {
        QColor(0x3b, 0x82, 0xc4),   // reads
        QColor(0xe0, 0x8a, 0x1e),   // writes
        QColor(0xd6, 0x45, 0x3d),   // interrupts
        QColor(0x3f, 0x9d, 0x5a),   // probe points
        QColor(0x8e, 0x5b, 0xc8)    // DMA
    };
    return c[lane];
}

} // namespace

TimelineWidget::TimelineWidget(const QString& name, QWidget* parent)
    : QWidget(parent), name_(name), doc_(nullptr), cursor_(0), marker_on_(false),
      marker_(0), t0_(0), t1_(1), axis_end_(0), pressed_(false), dragged_(false), press_t0_(0), press_t1_(1) {
    setFocusPolicy(Qt::StrongFocus);
    setAutoFillBackground(true);
    setAccessibleName(QStringLiteral("Timeline %1").arg(name));
    setMouseTracking(false);
}

void TimelineWidget::setDoc(const rwhost::TraceDoc* doc) {
    doc_    = doc;
    cursor_ = 0;
    marker_on_ = false;
    t0_ = 0;
    t1_ = fullEnd();
    update();
}

void TimelineWidget::setCaption(const QString& caption) {
    caption_ = caption;
    update();
}

quint64 TimelineWidget::fullEnd() const {
    const quint64 own = doc_ ? doc_->duration() : 0;
    const quint64 d   = std::max(own, axis_end_);
    return d > 0 ? d : 1;
}

void TimelineWidget::setAxisEnd(quint64 end) {
    axis_end_ = end;
    clampRange();
    update();
}

void TimelineWidget::reveal(std::size_t event) {
    if (!doc_ || event >= doc_->events().size()) {
        return;
    }
    const quint64 t = doc_->events()[event].ts;
    if (t >= t0_ && t <= t1_) {
        return;
    }
    const quint64 span = t1_ - t0_;
    const quint64 half = span / 2;
    t0_ = t > half ? t - half : 0;
    t1_ = t0_ + span;
    if (t1_ > fullEnd()) {
        t1_ = fullEnd();
        t0_ = t1_ > span ? t1_ - span : 0;
    }
    update();
    Q_EMIT rangeChanged(t0_, t1_);
}

void TimelineWidget::setCursor(std::size_t event) {
    if (doc_ && !doc_->events().empty()) {
        cursor_ = std::min(event, doc_->events().size() - 1);
    } else {
        cursor_ = 0;
    }
    update();
}

void TimelineWidget::setMarker(bool on, std::size_t event) {
    marker_on_ = on;
    marker_    = event;
    update();
}

void TimelineWidget::clampRange() {
    const quint64 full = fullEnd();
    if (t1_ > full) { t1_ = full; }
    if (t0_ >= t1_) { t0_ = t1_ > 0 ? t1_ - 1 : 0; }
    if (t1_ - t0_ < std::min(kMinSpan, full)) {
        // Too narrow to mean anything: widen about the middle.
        const quint64 want = std::min(kMinSpan, full);
        const quint64 mid  = t0_ + (t1_ - t0_) / 2;
        t0_ = mid > want / 2 ? mid - want / 2 : 0;
        t1_ = t0_ + want;
        if (t1_ > full) { t1_ = full; t0_ = full - want; }
    }
}

void TimelineWidget::setRange(quint64 t0, quint64 t1) {
    t0_ = t0;
    t1_ = t1;
    clampRange();
    update();
}

int TimelineWidget::plotLeft() const { return kLabelWidth; }

int TimelineWidget::plotWidth() const {
    return std::max(1, width() - kLabelWidth - kRightMargin);
}

quint64 TimelineWidget::timeAtX(int x) const {
    const int px = std::clamp(x - plotLeft(), 0, plotWidth());
    const long double f = (long double)px / (long double)plotWidth();
    return t0_ + (quint64)(f * (long double)(t1_ - t0_));
}

int TimelineWidget::xAtTime(quint64 t) const {
    if (t <= t0_) { return plotLeft(); }
    if (t >= t1_) { return plotLeft() + plotWidth(); }
    const long double f = (long double)(t - t0_) / (long double)(t1_ - t0_);
    return plotLeft() + (int)std::lround(f * (long double)plotWidth());
}

QSize TimelineWidget::sizeHint() const {
    return QSize(720, kTitleHeight + kRulerHeight + rwhost::kLaneCount * kRowHeight + kBottomMargin);
}

QSize TimelineWidget::minimumSizeHint() const {
    return QSize(320, sizeHint().height());
}

void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QPalette& pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Base));
    p.setPen(pal.color(QPalette::Text));

    QFont bold = font();
    bold.setBold(true);
    p.setFont(bold);
    p.drawText(QRect(6, 0, width() - 12, kTitleHeight), Qt::AlignVCenter | Qt::AlignLeft,
               caption_.isEmpty() ? name_ : QStringLiteral("%1   %2").arg(name_, caption_));
    p.setFont(font());

    if (!doc_ || doc_->events().empty()) {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(rect(), Qt::AlignCenter, QStringLiteral("no trace loaded"));
        return;
    }

    const std::vector<rwd::Event>& events = doc_->events();
    const int top   = kTitleHeight + kRulerHeight;
    const int left  = plotLeft();
    const int w     = plotWidth();

    // The ruler: a handful of cycle counts along the top of the plot.
    p.setPen(pal.color(QPalette::Mid));
    const QFontMetrics fm(font());
    for (int i = 0; i <= 4; ++i) {
        const int x = left + (w * i) / 4;
        const quint64 t = t0_ + (quint64)((long double)(t1_ - t0_) * i / 4.0L);
        p.drawLine(x, kTitleHeight + kRulerHeight - 4, x, top);
        const QString s = QString::number(t);
        const int tw = fm.horizontalAdvance(s);
        int tx = x - tw / 2;
        tx = std::clamp(tx, left - 4, left + w - tw + 4);
        p.drawText(tx, kTitleHeight + kRulerHeight - 6, s);
    }
    p.drawText(QRect(6, kTitleHeight, kLabelWidth - 12, kRulerHeight), Qt::AlignVCenter | Qt::AlignRight,
               QStringLiteral("cycle"));

    // One row per lane, as bars as tall as the slice is busy (square root, so a
    // single interrupt among a thousand reads is still there to be seen).
    const std::size_t n = std::max<std::size_t>(1, (std::size_t)w / kPxPerBucket);
    const std::vector<rwhost::Bucket> buckets = rwhost::bucketize(events, t0_, t1_, n);
    for (int lane = 0; lane < rwhost::kLaneCount; ++lane) {
        const int y = top + lane * kRowHeight;
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRect(6, y, kLabelWidth - 12, kRowHeight), Qt::AlignVCenter | Qt::AlignRight,
                   QString::fromUtf8(rwhost::lane_name((rwhost::Lane)lane)));
        p.fillRect(QRect(left, y + kRowHeight - 2, w, 1), pal.color(QPalette::Midlight));

        quint32 peak = 0;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            peak = std::max(peak, buckets[i].n[lane]);
        }
        if (peak == 0) {
            continue;
        }
        const QColor c = laneColor(lane);
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            const quint32 count = buckets[i].n[lane];
            if (count == 0) { continue; }
            const double frac = std::sqrt((double)count / (double)peak);
            const int h = std::max(2, (int)std::lround(frac * (kRowHeight - 4)));
            const int x0 = left + (int)((long long)w * (long long)i / (long long)buckets.size());
            const int x1 = left + (int)((long long)w * (long long)(i + 1) / (long long)buckets.size());
            p.fillRect(QRect(x0, y + kRowHeight - 2 - h, std::max(1, x1 - x0), h), c);
        }
    }

    // The first difference between two traces, and the cursor on top of it.
    const int bottom = top + rwhost::kLaneCount * kRowHeight;
    if (marker_on_ && marker_ < events.size()) {
        const quint64 t = events[marker_].ts;
        if (t >= t0_ && t <= t1_) {
            QPen pen(QColor(0xd6, 0x45, 0x3d));
            pen.setStyle(Qt::DashLine);
            pen.setWidth(2);
            p.setPen(pen);
            const int x = xAtTime(t);
            p.drawLine(x, kTitleHeight, x, bottom);
        }
    }
    if (cursor_ < events.size()) {
        const quint64 t = events[cursor_].ts;
        if (t >= t0_ && t <= t1_) {
            QPen pen(pal.color(QPalette::Highlight));
            pen.setWidth(2);
            p.setPen(pen);
            const int x = xAtTime(t);
            p.drawLine(x, kTitleHeight + 2, x, bottom);
        }
    }
}

void TimelineWidget::wheelEvent(QWheelEvent* e) {
    if (!doc_) { e->ignore(); return; }
    const int steps = e->angleDelta().y();
    if (steps == 0) { e->ignore(); return; }

    // About the pointer: the cycle under it stays under it.
    const quint64 anchor = timeAtX((int)std::lround(e->position().x()));
    const long double factor = std::pow(0.8L, (long double)steps / 120.0L);   // in: narrower
    const long double span   = (long double)(t1_ - t0_) * factor;
    const long double left   = (long double)(anchor - t0_) / (long double)(t1_ - t0_);
    const long double nt0    = (long double)anchor - left * span;
    const long double nt1    = nt0 + span;

    const quint64 full = fullEnd();
    t0_ = nt0 < 0 ? 0 : (quint64)nt0;
    t1_ = nt1 < 1 ? 1 : (nt1 > (long double)full ? full : (quint64)std::llround(nt1));
    clampRange();
    update();
    Q_EMIT rangeChanged(t0_, t1_);
    e->accept();
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || !doc_) { e->ignore(); return; }
    setFocus();
    pressed_   = true;
    dragged_   = false;
    press_pos_ = e->pos();
    press_t0_  = t0_;
    press_t1_  = t1_;
    e->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
    if (!pressed_) { e->ignore(); return; }
    const QPoint d = e->pos() - press_pos_;
    if (!dragged_ && std::abs(d.x()) < 4 && std::abs(d.y()) < 4) {
        return;                       // still a click
    }
    dragged_ = true;
    // Dragging right moves the run right, so the range moves left.
    const long double span  = (long double)(press_t1_ - press_t0_);
    const long double shift = (long double)d.x() * span / (long double)plotWidth();
    long double nt0 = (long double)press_t0_ - shift;
    const long double full = (long double)fullEnd();
    nt0 = std::clamp(nt0, (long double)0, full - span);
    t0_ = (quint64)nt0;
    t1_ = t0_ + (quint64)span;
    update();
    Q_EMIT rangeChanged(t0_, t1_);
    e->accept();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (!pressed_ || e->button() != Qt::LeftButton) { e->ignore(); return; }
    pressed_ = false;
    if (!dragged_ && doc_ && !doc_->events().empty()) {
        // The first event at or after the cycle under the pointer.
        const quint64 t = timeAtX(e->pos().x());
        std::size_t ev = rwhost::first_event_at_or_after(doc_->events(), t);
        ev = std::min(ev, doc_->events().size() - 1);
        cursor_ = ev;
        update();
        Q_EMIT cursorChanged(ev);
    }
    e->accept();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (!doc_) { e->ignore(); return; }
    t0_ = 0;
    t1_ = fullEnd();
    update();
    Q_EMIT rangeChanged(t0_, t1_);
    e->accept();
}

void TimelineWidget::moveCursor(long long delta) {
    if (!doc_ || doc_->events().empty()) { return; }
    const long long last = (long long)doc_->events().size() - 1;
    const long long next = std::clamp((long long)cursor_ + delta, 0LL, last);
    if ((std::size_t)next == cursor_) {
        return;                       // at an end already: nothing moved
    }
    cursor_ = (std::size_t)next;
    update();
    Q_EMIT cursorChanged((qulonglong)cursor_);
}

void TimelineWidget::keyPressEvent(QKeyEvent* e) {
    const long long big = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    switch (e->key()) {
        case Qt::Key_Left:  moveCursor(-big); break;
        case Qt::Key_Right: moveCursor(big);  break;
        case Qt::Key_Home:  moveCursor(-(long long)cursor_); break;
        case Qt::Key_End:   moveCursor((long long)(doc_ ? doc_->events().size() : 0)); break;
        default: QWidget::keyPressEvent(e); return;
    }
    e->accept();
}
