// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "eventmodel.h"

#include <QBrush>
#include <QColor>
#include <QFont>

#include "rewind/describe.h"

EventModel::EventModel(QObject* parent)
    : QAbstractTableModel(parent), doc_(nullptr), is_a_(true) {}

void EventModel::setDoc(const rwhost::TraceDoc* doc) {
    beginResetModel();
    doc_ = doc;
    marks_.clear();
    endResetModel();
}

void EventModel::setMarks(const std::vector<unsigned char>& marks, bool is_a) {
    marks_ = marks;
    is_a_  = is_a;
    if (rowCount() > 0) {
        Q_EMIT dataChanged(index(0, 0), index(rowCount() - 1, kColCount - 1),
                         QList<int>() << Qt::BackgroundRole);
    }
}

void EventModel::clearMarks() {
    setMarks(std::vector<unsigned char>(), is_a_);
}

EventModel::Mark EventModel::markAt(std::size_t row) const {
    if (row >= marks_.size()) {
        return kMarkNone;
    }
    return marks_[row] == kMarkOnlyHere ? kMarkOnlyHere
         : marks_[row] == kMarkOnlyThere ? kMarkOnlyThere : kMarkNone;
}

int EventModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid() || !doc_) {
        return 0;
    }
    return (int)doc_->events().size();
}

int EventModel::columnCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : kColCount;
}

QVariant EventModel::data(const QModelIndex& index, int role) const {
    if (!doc_ || !index.isValid() || index.row() < 0 ||
        (std::size_t)index.row() >= doc_->events().size()) {
        return QVariant();
    }
    const rwd::Event& e = doc_->events()[(std::size_t)index.row()];

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
            case kColIndex: return index.row();
            case kColCycle: return (qulonglong)e.ts;
            case kColEvent: return QString::fromStdString(rwhost::describe_event(e));
            default:        return QVariant();
        }
    }
    if (role == Qt::TextAlignmentRole && index.column() != kColEvent) {
        return (int)(Qt::AlignRight | Qt::AlignVCenter);
    }
    if (role == Qt::FontRole && index.column() == kColEvent) {
        QFont f;
        f.setStyleHint(QFont::Monospace);
        f.setFamily(QStringLiteral("monospace"));
        return f;
    }
    if (role == Qt::BackgroundRole) {
        switch (markAt((std::size_t)index.row())) {
            case kMarkOnlyHere:
                // Red where this is the first trace (what it did that the other
                // did not), green where it is the second.
                return QBrush(is_a_ ? QColor(214, 69, 61, 70) : QColor(63, 157, 90, 70));
            case kMarkOnlyThere:
            case kMarkNone:
            default:
                return QVariant();
        }
    }
    return QVariant();
}

QVariant EventModel::headerData(int section, Qt::Orientation orientation, int role) const {
    if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
        return QVariant();
    }
    switch (section) {
        case kColIndex: return QStringLiteral("#");
        case kColCycle: return QStringLiteral("cycle");
        case kColEvent: return QStringLiteral("event");
        default:        return QVariant();
    }
}
