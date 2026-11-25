// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.

#include "mainwindow.h"

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

#include "eventmodel.h"
#include "rewind/tracediff.h"
#include "sim/mcu.h"
#include "statepanel.h"
#include "timelinewidget.h"

namespace {

const char* sideName(int side) { return side == MainWindow::kSideA ? "A" : "B"; }

QString plural(std::size_t n, const char* one, const char* many) {
    return QStringLiteral("%1 %2").arg((qulonglong)n).arg(QLatin1String(n == 1 ? one : many));
}

// "#12" for one event, "#12–#15" for several.
QString range(std::size_t first, std::size_t n) {
    if (n <= 1) {
        return QStringLiteral("#%1").arg((qulonglong)first);
    }
    return QStringLiteral("#%1–#%2").arg((qulonglong)first).arg((qulonglong)(first + n - 1));
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), main_iters_(6000), syncing_(false), state_(nullptr),
      differences_(nullptr), tabs_(nullptr), status_(nullptr) {
    for (int i = 0; i < 2; ++i) {
        cursor_[i]   = 0;
        timeline_[i] = nullptr;
        model_[i]    = nullptr;
        table_[i]    = nullptr;
    }
    setWindowTitle(QStringLiteral("rewind"));

    QSplitter* vertical = new QSplitter(Qt::Vertical, this);

    // Timelines, one over the other so that the same cycle is at the same x.
    QWidget* top = new QWidget(vertical);
    QVBoxLayout* topLayout = new QVBoxLayout(top);
    topLayout->setContentsMargins(0, 0, 0, 0);
    topLayout->setSpacing(1);
    for (int i = 0; i < 2; ++i) {
        timeline_[i] = new TimelineWidget(QString::fromLatin1(sideName(i)), top);
        topLayout->addWidget(timeline_[i]);
        timeline_[i]->setVisible(i == 0);
        const Side side = (Side)i;
        connect(timeline_[i], &TimelineWidget::cursorChanged, this,
                [this, side](qulonglong ev) { setCursor(side, (std::size_t)ev); });
    }

    // Events of each trace side by side, and what the cursor says beside them.
    QSplitter* bottom = new QSplitter(Qt::Horizontal, vertical);
    for (int i = 0; i < 2; ++i) {
        model_[i] = new EventModel(this);
        table_[i] = new QTableView(bottom);
        table_[i]->setModel(model_[i]);
        table_[i]->setAccessibleName(QStringLiteral("Events %1").arg(QLatin1String(sideName(i))));
        table_[i]->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_[i]->setSelectionMode(QAbstractItemView::SingleSelection);
        table_[i]->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_[i]->setAlternatingRowColors(true);
        table_[i]->setWordWrap(false);
        table_[i]->verticalHeader()->hide();
        table_[i]->verticalHeader()->setDefaultSectionSize(19);
        table_[i]->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_[i]->horizontalHeader()->setStretchLastSection(true);
        table_[i]->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        table_[i]->setColumnWidth(EventModel::kColIndex, 58);
        table_[i]->setColumnWidth(EventModel::kColCycle, 88);
        table_[i]->setVisible(i == 0);
        const Side side = (Side)i;
        connect(table_[i]->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
                [this, side](const QModelIndex& cur, const QModelIndex&) {
                    if (!syncing_ && cur.isValid()) {
                        setCursor(side, (std::size_t)cur.row());
                    }
                });
    }

    tabs_ = new QTabWidget(bottom);
    state_ = new StatePanel(tabs_);
    differences_ = new QListWidget(tabs_);
    differences_->setAccessibleName(QStringLiteral("Differences"));
    tabs_->addTab(state_, QStringLiteral("State"));
    tabs_->addTab(differences_, QStringLiteral("Differences"));
    connect(differences_, &QListWidget::itemActivated, this, &MainWindow::onDifferenceActivated);
    connect(differences_, &QListWidget::itemClicked, this, &MainWindow::onDifferenceActivated);

    bottom->setStretchFactor(0, 3);
    bottom->setStretchFactor(1, 3);
    bottom->setStretchFactor(2, 2);
    vertical->setStretchFactor(0, 0);
    vertical->setStretchFactor(1, 1);
    vertical->setCollapsible(0, false);
    setCentralWidget(vertical);

    status_ = new QLabel(this);
    statusBar()->addWidget(status_, 1);

    QMenu* file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction* openA = file->addAction(QStringLiteral("Open trace &A…"));
    openA->setShortcut(QKeySequence::Open);
    connect(openA, &QAction::triggered, this, [this] { onOpen(kSideA); });
    QAction* openB = file->addAction(QStringLiteral("Open trace &B to compare…"));
    connect(openB, &QAction::triggered, this, [this] { onOpen(kSideB); });
    file->addSeparator();
    QAction* quit = file->addAction(QStringLiteral("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu* go = menuBar()->addMenu(QStringLiteral("&Go"));
    QAction* first = go->addAction(QStringLiteral("&First difference"));
    first->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
    connect(first, &QAction::triggered, this, [this] { goFirstDifference(); });
    QAction* next = go->addAction(QStringLiteral("&Next difference"));
    next->setShortcut(QKeySequence(Qt::Key_F3));
    connect(next, &QAction::triggered, this, [this] { goNextDifference(); });
    QAction* prev = go->addAction(QStringLiteral("&Previous difference"));
    prev->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F3));
    connect(prev, &QAction::triggered, this, [this] { goPreviousDifference(); });

    resize(1180, 720);
    refresh();
}

MainWindow::~MainWindow() {}

void MainWindow::onOpen(int side) {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open trace %1").arg(QLatin1String(sideName(side))), QString(),
        QStringLiteral("Traces (*.rwd);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QString error;
    if (!openFile((Side)side, path, &error)) {
        QMessageBox::critical(this, QStringLiteral("rewind"), error);
    }
}

bool MainWindow::openFile(Side side, const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) { *error = QStringLiteral("cannot open %1: %2").arg(path, f.errorString()); }
        return false;
    }
    const QByteArray raw = f.readAll();
    if (f.error() != QFileDevice::NoError) {
        if (error) { *error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString()); }
        return false;
    }
    std::vector<rwd::u8> bytes(raw.size());
    std::copy(raw.constData(), raw.constData() + raw.size(), bytes.begin());
    return openBytes(side, bytes, QFileInfo(path).fileName(), error);
}

bool MainWindow::openBytes(Side side, const std::vector<rwd::u8>& bytes, const QString& name,
                           QString* error) {
    std::unique_ptr<rwhost::TraceDoc> d(new rwhost::TraceDoc());
    if (factory_) {
        d->set_firmware_factory(factory_);
    }
    if (!d->open(bytes, main_iters_)) {
        if (error) {
            *error = QStringLiteral("%1: %2").arg(name, QString::fromStdString(d->error()));
        }
        return false;
    }
    doc_[side]  = std::move(d);
    name_[side] = name;
    cursor_[side] = 0;
    rebuild();
    return true;
}

QString MainWindow::captionFor(Side side) const {
    const rwhost::TraceDoc* d = doc_[side].get();
    if (!d) {
        return QString();
    }
    const QString what = d->has_firmware()
                             ? QStringLiteral("%1 firmware").arg(QLatin1String(d->variant_name()))
                             : QStringLiteral("unknown firmware");
    return QStringLiteral("%1  ·  %2  ·  seed %3  ·  %4  ·  %5")
        .arg(name_[side], what)
        .arg((qulonglong)d->header().seed)
        .arg(plural(d->events().size(), "event", "events"))
        .arg(plural((std::size_t)d->duration(), "cycle", "cycles"));
}

// Everything that depends on which traces are open, and not on the cursor.
void MainWindow::rebuild() {
    diff_ = rwhost::TraceDiff();
    regions_.clear();
    differences_->clear();

    quint64 axis = 0;
    for (int i = 0; i < 2; ++i) {
        if (doc_[i]) {
            axis = std::max<quint64>(axis, doc_[i]->duration());
        }
    }

    for (int i = 0; i < 2; ++i) {
        const rwhost::TraceDoc* d = doc_[i].get();
        model_[i]->setDoc(d);
        timeline_[i]->setDoc(d);
        timeline_[i]->setAxisEnd(axis);
        timeline_[i]->setRange(0, axis > 0 ? axis : 1);
        timeline_[i]->setCaption(captionFor((Side)i));
        timeline_[i]->setMarker(false);
        timeline_[i]->setVisible(i == 0 || d != nullptr);
        table_[i]->setVisible(i == 0 || d != nullptr);
        cursor_[i] = std::min(cursor_[i], d && !d->events().empty() ? d->events().size() - 1 : 0);
    }

    if (comparing()) {
        rwhost::DiffOptions opt;
        opt.ignore_value_at.push_back(sim::kSystickCnt);
        diff_ = rwhost::diff_traces(doc_[kSideA]->events(), doc_[kSideB]->events(), opt);

        std::vector<unsigned char> ma(doc_[kSideA]->events().size(), EventModel::kMarkNone);
        std::vector<unsigned char> mb(doc_[kSideB]->events().size(), EventModel::kMarkNone);
        bool open_region = false;
        for (std::size_t h = 0; h < diff_.hunks.size(); ++h) {
            const rwhost::DiffHunk& k = diff_.hunks[h];
            if (k.kind == rwhost::kHunkEqual) {
                open_region = false;
                continue;
            }
            if (!open_region) {
                Region r = {k.a, 0, k.b, 0};
                regions_.push_back(r);
                open_region = true;
            }
            regions_.back().na += k.na;
            regions_.back().nb += k.nb;
            for (std::size_t i = 0; i < k.na; ++i) { ma[k.a + i] = EventModel::kMarkOnlyHere; }
            for (std::size_t i = 0; i < k.nb; ++i) { mb[k.b + i] = EventModel::kMarkOnlyHere; }
        }
        for (std::size_t i = 0; i < regions_.size(); ++i) {
            const Region& r = regions_[i];
            QString text;
            if (r.na > 0 && r.nb > 0) {
                text = QStringLiteral("A %1 (%2)  ⇄  B %3 (%4)")
                           .arg(range(r.a, r.na), plural(r.na, "event", "events"),
                                range(r.b, r.nb), plural(r.nb, "event", "events"));
            } else if (r.na > 0) {
                text = QStringLiteral("only in A  ·  %1 (%2)")
                           .arg(range(r.a, r.na), plural(r.na, "event", "events"));
            } else {
                text = QStringLiteral("only in B  ·  %1 (%2)")
                           .arg(range(r.b, r.nb), plural(r.nb, "event", "events"));
            }
            differences_->addItem(text);
        }
        model_[kSideA]->setMarks(ma, true);
        model_[kSideB]->setMarks(mb, false);

        if (!diff_.identical) {
            timeline_[kSideA]->setMarker(diff_.first < doc_[kSideA]->events().size(), diff_.first);
            timeline_[kSideB]->setMarker(diff_.first < doc_[kSideB]->events().size(), diff_.first);
        }
        cursor_[kSideB] = rwhost::map_a_to_b(diff_, cursor_[kSideA], doc_[kSideB]->events().size());
    } else {
        model_[0]->clearMarks();
        model_[1]->clearMarks();
    }

    tabs_->setTabText(1, comparing() ? QStringLiteral("Differences (%1)").arg(regions_.size())
                                     : QStringLiteral("Differences"));
    tabs_->setTabEnabled(1, comparing());
    setWindowTitle(comparing() ? QStringLiteral("rewind — %1 vs %2").arg(name_[0], name_[1])
                 : doc_[0]     ? QStringLiteral("rewind — %1").arg(name_[0])
                               : QStringLiteral("rewind"));
    refresh();
}

QString MainWindow::summary() const {
    if (!doc_[kSideA] && !doc_[kSideB]) {
        return QStringLiteral("no trace open — File ▸ Open trace A");
    }
    if (!comparing()) {
        return QStringLiteral("event %1 of %2")
            .arg((qulonglong)cursor_[kSideA])
            .arg((qulonglong)doc_[kSideA]->events().size());
    }
    if (diff_.identical) {
        return QStringLiteral("the two traces are identical (%1)")
            .arg(plural(diff_.equal, "event", "events"));
    }
    return QStringLiteral("agree for %1, then differ  ·  %2 only in A, %3 only in B%4")
        .arg(plural(diff_.first, "event", "events"))
        .arg((qulonglong)diff_.only_a)
        .arg((qulonglong)diff_.only_b)
        .arg(diff_.minimal ? QString() : QStringLiteral("  ·  alignment not minimal"));
}

// Everything that depends on the cursor.
void MainWindow::refresh() {
    syncing_ = true;
    for (int i = 0; i < 2; ++i) {
        const rwhost::TraceDoc* d = doc_[i].get();
        timeline_[i]->setCursor(cursor_[i]);
        if (d && !d->events().empty()) {
            timeline_[i]->reveal(cursor_[i]);
            const QModelIndex idx = model_[i]->index((int)cursor_[i], 0);
            table_[i]->selectionModel()->setCurrentIndex(
                idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            table_[i]->scrollTo(idx, QAbstractItemView::PositionAtCenter);
        }
    }
    syncing_ = false;

    rwhost::Snapshot sa, sb;
    const bool has_a = doc_[kSideA] && doc_[kSideA]->snapshot_at(cursor_[kSideA], &sa);
    const bool has_b = doc_[kSideB] && doc_[kSideB]->snapshot_at(cursor_[kSideB], &sb);
    state_->setStates(has_a ? &sa : nullptr, has_b ? &sb : nullptr, comparing());

    QString line = summary();
    if (comparing()) {
        line += QStringLiteral("  ·  cursor A #%1, B #%2  ·  %3 differ")
                    .arg((qulonglong)cursor_[kSideA])
                    .arg((qulonglong)cursor_[kSideB])
                    .arg(plural((std::size_t)state_->differingRows(), "variable", "variables"));
    }
    status_->setText(line);
}

void MainWindow::setCursor(Side side, std::size_t event) {
    const rwhost::TraceDoc* d = doc_[side].get();
    if (!d || d->events().empty()) {
        return;
    }
    event = std::min(event, d->events().size() - 1);
    cursor_[side] = event;
    if (comparing()) {
        if (side == kSideA) {
            cursor_[kSideB] =
                rwhost::map_a_to_b(diff_, event, doc_[kSideB]->events().size());
        } else {
            cursor_[kSideA] =
                rwhost::map_b_to_a(diff_, event, doc_[kSideA]->events().size());
        }
    }
    refresh();
}

// Puts the cursor where a difference starts, in each trace. A difference that
// begins past the end of a trace (the other one went on) shows that trace's
// last event.
void MainWindow::jumpToRegion(std::size_t region) {
    if (!comparing() || region >= regions_.size()) {
        return;
    }
    const Region& r = regions_[region];
    cursor_[kSideA] = std::min(r.a, doc_[kSideA]->events().size() - 1);
    cursor_[kSideB] = std::min(r.b, doc_[kSideB]->events().size() - 1);
    refresh();
}

void MainWindow::onDifferenceActivated(QListWidgetItem* item) {
    const int row = differences_->row(item);
    if (row >= 0) {
        jumpToRegion((std::size_t)row);
    }
}

void MainWindow::goFirstDifference() {
    jumpToRegion(0);
}

void MainWindow::goNextDifference() {
    if (!comparing()) { return; }
    const std::size_t last = doc_[kSideA]->events().size() - 1;
    for (std::size_t i = 0; i < regions_.size(); ++i) {
        if (std::min(regions_[i].a, last) > cursor_[kSideA]) {
            jumpToRegion(i);
            return;
        }
    }
}

void MainWindow::goPreviousDifference() {
    if (!comparing()) { return; }
    const std::size_t last = doc_[kSideA]->events().size() - 1;
    for (std::size_t i = regions_.size(); i > 0; --i) {
        if (std::min(regions_[i - 1].a, last) < cursor_[kSideA]) {
            jumpToRegion(i - 1);
            return;
        }
    }
}
