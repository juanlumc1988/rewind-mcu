// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "statepanel.h"

#include <QBrush>
#include <QColor>
#include <QHeaderView>
#include <QStringList>

#include <cstddef>

namespace {

struct Field {
    const char* name;
    rwd::u32 fw::State::*member;
};

// The scalar fields, in the order the firmware declares them. The ring is shown
// as one hex row below, because sixteen rows of one byte each is no help.
const Field kFields[] = {
    {"head",       &fw::State::head},
    {"tail",       &fw::State::tail},
    {"pending",    &fw::State::pending},
    {"consumed",   &fw::State::consumed},
    {"checksum",   &fw::State::checksum},
    {"last_stamp", &fw::State::last_stamp},
    {"overflows",  &fw::State::overflows},
    {"dma_tail",   &fw::State::dma_tail},
    {"dma_lost",   &fw::State::dma_lost},
};
const int kFieldCount = (int)(sizeof(kFields) / sizeof(kFields[0]));

QString ringText(const fw::State& s) {
    QString out;
    for (rwd::u32 i = 0; i < fw::kRingSize; ++i) {
        if (i != 0) {
            out += QLatin1Char(' ');
        }
        out += QStringLiteral("%1").arg((uint)s.ring[i], 2, 16, QLatin1Char('0')).toUpper();
    }
    return out;
}

QTableWidgetItem* cell(const QString& text, bool right) {
    QTableWidgetItem* it = new QTableWidgetItem(text);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    if (right) {
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    return it;
}

} // namespace

StatePanel::StatePanel(QWidget* parent) : QTableWidget(parent), differing_(0) {
    setAccessibleName(QStringLiteral("Firmware state"));
    setColumnCount(2);
    setRowCount(kFieldCount + 1);
    setHorizontalHeaderLabels(QStringList() << QStringLiteral("A") << QStringLiteral("B"));

    QStringList rows;
    for (int i = 0; i < kFieldCount; ++i) {
        rows << QString::fromLatin1(kFields[i].name);
    }
    rows << QStringLiteral("ring");
    setVerticalHeaderLabels(rows);

    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    setWordWrap(false);
    setStates(false, fw::State(), false, fw::State(), true);
}

int StatePanel::rowOf(const QString& name) const {
    for (int i = 0; i < rowCount(); ++i) {
        if (verticalHeaderItem(i) && verticalHeaderItem(i)->text() == name) {
            return i;
        }
    }
    return -1;
}

void StatePanel::setStates(bool has_a, const fw::State& a, bool has_b, const fw::State& b,
                           bool two_columns) {
    setColumnHidden(1, !two_columns);
    differing_ = 0;

    const QBrush hot(QColor(214, 69, 61, 80));
    const bool compare = two_columns && has_a && has_b;

    for (int r = 0; r <= kFieldCount; ++r) {
        const bool ring = (r == kFieldCount);
        QString ta = QStringLiteral("-");
        QString tb = QStringLiteral("-");
        bool differs = false;
        if (ring) {
            if (has_a) { ta = ringText(a); }
            if (has_b) { tb = ringText(b); }
            differs = compare && ringText(a) != ringText(b);
        } else {
            if (has_a) { ta = QString::number(a.*(kFields[r].member)); }
            if (has_b) { tb = QString::number(b.*(kFields[r].member)); }
            differs = compare && (a.*(kFields[r].member) != b.*(kFields[r].member));
        }
        QTableWidgetItem* ia = cell(ta, !ring);
        QTableWidgetItem* ib = cell(tb, !ring);
        if (differs) {
            ia->setBackground(hot);
            ib->setBackground(hot);
            ++differing_;
        }
        setItem(r, 0, ia);
        setItem(r, 1, ib);
    }
}
