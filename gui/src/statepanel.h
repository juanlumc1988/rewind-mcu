// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The firmware's variables at the cursor, one column per trace. A row that
// differs between the two is tinted: after the first difference that is
// usually where the bug shows.
//
// The rows are whatever the firmware describes (Firmware::describe_state), so
// this is not specific to the example. Array elements -- fields named
// `name[index]` -- are shown together in one row, and below the variables is a
// row for the state's hash, which is all there is of a firmware that does not
// describe its fields.

#ifndef REWIND_GUI_STATEPANEL_H
#define REWIND_GUI_STATEPANEL_H

#include <QTableWidget>

#include "rewind/timeline.h"

class StatePanel : public QTableWidget {
    Q_OBJECT

public:
    explicit StatePanel(QWidget* parent = nullptr);

    // A side with no state (no firmware to run, or no trace) is null and shows
    // dashes. `two_columns` shows the B column and compares the two.
    void setStates(const rwhost::Snapshot* a, const rwhost::Snapshot* b, bool two_columns);

    // Rows that differ at the moment, not counting the hash (it differs
    // whenever anything does), for tests and for the status line.
    int differingRows() const { return differing_; }
    int rowOf(const QString& name) const;

private:
    int differing_;
};

#endif // REWIND_GUI_STATEPANEL_H
