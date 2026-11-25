// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The firmware's variables at the cursor, one column per trace. A row that
// differs between the two is tinted: after the first difference that is
// usually where the bug shows.

#ifndef REWIND_GUI_STATEPANEL_H
#define REWIND_GUI_STATEPANEL_H

#include <QTableWidget>

#include "fw/rx_pump.h"

class StatePanel : public QTableWidget {
    Q_OBJECT

public:
    explicit StatePanel(QWidget* parent = nullptr);

    // A side with no state (no firmware to run, or no trace) shows dashes.
    void setStates(bool has_a, const fw::State& a, bool has_b, const fw::State& b,
                   bool two_columns);

    // Names of the rows that differ at the moment, for tests and for the status line.
    int differingRows() const { return differing_; }
    int rowOf(const QString& name) const;

private:
    int differing_;
};

#endif // REWIND_GUI_STATEPANEL_H
