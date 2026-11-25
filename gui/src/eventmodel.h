// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The events of one trace as a table: number, cycle, and the event in words.
// Rows the other trace does not have are tinted, so the places two runs part
// ways stand out when scrolling past them.

#ifndef REWIND_GUI_EVENTMODEL_H
#define REWIND_GUI_EVENTMODEL_H

#include <QAbstractTableModel>

#include <cstddef>
#include <vector>

#include "rewind/tracedoc.h"

class EventModel : public QAbstractTableModel {
    Q_OBJECT

public:
    enum Column { kColIndex = 0, kColCycle = 1, kColEvent = 2, kColCount = 3 };

    // What a row is, against the other trace.
    enum Mark { kMarkNone = 0, kMarkOnlyHere = 1, kMarkOnlyThere = 2 };

    explicit EventModel(QObject* parent = nullptr);

    void setDoc(const rwhost::TraceDoc* doc);
    // One entry per event; shorter is read as "none" for the rest. `here` is the
    // colour of rows only this trace has.
    void setMarks(const std::vector<unsigned char>& marks, bool is_a);
    void clearMarks();

    Mark markAt(std::size_t row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;

private:
    const rwhost::TraceDoc*    doc_;
    std::vector<unsigned char> marks_;
    bool                       is_a_;
};

#endif // REWIND_GUI_EVENTMODEL_H
