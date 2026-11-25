// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "statepanel.h"

#include <QBrush>
#include <QColor>
#include <QHeaderView>
#include <QRegularExpression>
#include <QStringList>

#include <cstddef>
#include <vector>

namespace {

// One row as it will be shown: a name and the text of its value.
struct Row {
    QString name;
    QString text;
};

// The fields of one snapshot as rows, in the order the firmware listed them.
// Elements of an array (`name[3]`) become one row, `name`.
std::vector<Row> rowsOf(const rwhost::Snapshot& s) {
    static const QRegularExpression element(QStringLiteral("^(.+)\\[(\\d+)\\]$"));

    std::vector<Row>                  rows;
    std::vector<std::vector<quint64>> arrays;     // parallel to rows; empty for a scalar
    std::vector<bool>                 is_array;

    for (std::size_t i = 0; i < s.fields.size(); ++i) {
        const QString name = QString::fromStdString(s.fields[i].name);
        const QRegularExpressionMatch m = element.match(name);
        if (m.hasMatch()) {
            const QString base = m.captured(1);
            std::size_t at = rows.size();
            for (std::size_t k = 0; k < rows.size(); ++k) {
                if (is_array[k] && rows[k].name == base) {
                    at = k;
                    break;
                }
            }
            if (at == rows.size()) {
                Row r;
                r.name = base;
                rows.push_back(r);
                arrays.push_back(std::vector<quint64>());
                is_array.push_back(true);
            }
            arrays[at].push_back(s.fields[i].value);
        } else {
            Row r;
            r.name = name;
            r.text = QString::number((qulonglong)s.fields[i].value);
            rows.push_back(r);
            arrays.push_back(std::vector<quint64>());
            is_array.push_back(false);
        }
    }

    for (std::size_t k = 0; k < rows.size(); ++k) {
        if (!is_array[k]) {
            continue;
        }
        // Bytes read best in hex; anything wider as the numbers they are.
        bool bytes = true;
        for (std::size_t j = 0; j < arrays[k].size(); ++j) {
            bytes = bytes && arrays[k][j] <= 0xFF;
        }
        QStringList parts;
        for (std::size_t j = 0; j < arrays[k].size(); ++j) {
            parts << (bytes ? QStringLiteral("%1").arg((uint)arrays[k][j], 2, 16, QLatin1Char('0')).toUpper()
                            : QString::number((qulonglong)arrays[k][j]));
        }
        rows[k].text = parts.join(QLatin1Char(' '));
    }
    return rows;
}

QTableWidgetItem* cell(const QString& text, bool right) {
    QTableWidgetItem* it = new QTableWidgetItem(text);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    if (right) {
        it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    return it;
}

const char* kHashRow = "state hash";

} // namespace

StatePanel::StatePanel(QWidget* parent) : QTableWidget(parent), differing_(0) {
    setAccessibleName(QStringLiteral("Firmware state"));
    setColumnCount(2);
    setHorizontalHeaderLabels(QStringList() << QStringLiteral("A") << QStringLiteral("B"));
    setSelectionMode(QAbstractItemView::NoSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    setWordWrap(false);
    setStates(nullptr, nullptr, true);
}

int StatePanel::rowOf(const QString& name) const {
    for (int i = 0; i < rowCount(); ++i) {
        if (verticalHeaderItem(i) && verticalHeaderItem(i)->text() == name) {
            return i;
        }
    }
    return -1;
}

void StatePanel::setStates(const rwhost::Snapshot* a, const rwhost::Snapshot* b,
                           bool two_columns) {
    setColumnHidden(1, !two_columns);
    differing_ = 0;

    const std::vector<Row> ra = a ? rowsOf(*a) : std::vector<Row>();
    const std::vector<Row> rb = b ? rowsOf(*b) : std::vector<Row>();

    // The names in the order the first side lists them, then any only the
    // second has. Two states of one firmware list the same.
    QStringList names;
    for (std::size_t i = 0; i < ra.size(); ++i) { names << ra[i].name; }
    for (std::size_t i = 0; i < rb.size(); ++i) {
        if (!names.contains(rb[i].name)) { names << rb[i].name; }
    }
    auto find = [](const std::vector<Row>& rows, const QString& name, QString* text) {
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].name == name) { *text = rows[i].text; return true; }
        }
        return false;
    };

    const QBrush hot(QColor(214, 69, 61, 80));
    const bool compare = two_columns && a && b;

    clearContents();
    setRowCount(names.size() + 1);
    QStringList header = names;
    header << QString::fromLatin1(kHashRow);
    setVerticalHeaderLabels(header);

    for (int r = 0; r < names.size(); ++r) {
        QString ta = QStringLiteral("-");
        QString tb = QStringLiteral("-");
        const bool in_a = find(ra, names[r], &ta);
        const bool in_b = find(rb, names[r], &tb);
        const bool differs = compare && (in_a != in_b || ta != tb);
        const bool is_array = ta.contains(QLatin1Char(' ')) || tb.contains(QLatin1Char(' '));
        QTableWidgetItem* ia = cell(ta, !is_array);
        QTableWidgetItem* ib = cell(tb, !is_array);
        if (differs) {
            ia->setBackground(hot);
            ib->setBackground(hot);
            ++differing_;
        }
        setItem(r, 0, ia);
        setItem(r, 1, ib);
    }

    // The hash: equal states always have equal hashes, so it is shown, and
    // tinted when it differs, but is not counted as a variable.
    const int last = names.size();
    const QString ha = a ? QStringLiteral("%1").arg((qulonglong)a->state_hash, 16, 16, QLatin1Char('0'))
                         : QStringLiteral("-");
    const QString hb = b ? QStringLiteral("%1").arg((qulonglong)b->state_hash, 16, 16, QLatin1Char('0'))
                         : QStringLiteral("-");
    QTableWidgetItem* xa = cell(ha, true);
    QTableWidgetItem* xb = cell(hb, true);
    if (compare && ha != hb) {
        xa->setBackground(hot);
        xb->setBackground(hot);
    }
    setItem(last, 0, xa);
    setItem(last, 1, xb);
}
