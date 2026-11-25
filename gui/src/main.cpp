// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
//   rewind-gui [A.rwd [B.rwd]] [--at EVENT] [--first-diff] [--iters N]
//              [--screenshot FILE [--size WxH]]
//
// With two traces it compares them. --screenshot renders the window to a PNG
// and exits, which is how the picture in the documentation is made and how a
// headless machine can show what the viewer shows (QT_QPA_PLATFORM=offscreen).

#include <QApplication>
#include <QCommandLineParser>
#include <QImage>
#include <QTextStream>

#include <cstdio>
#include <cstring>

#include "mainwindow.h"
#include "rewind/version.h"

int main(int argc, char** argv) {
    // Before the application exists: asking for the version must work on a
    // machine with no display, which QApplication would otherwise abort on.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::printf("rewind-gui %s\n", REWIND_VERSION);
            return 0;
        }
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("rewind-gui"));
    QApplication::setApplicationVersion(QStringLiteral(REWIND_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Look at a recorded trace, or compare two of them."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("A.rwd"), QStringLiteral("trace to open"));
    parser.addPositionalArgument(QStringLiteral("B.rwd"),
                                 QStringLiteral("second trace to compare against"),
                                 QStringLiteral("[B.rwd]"));
    parser.addOption(QCommandLineOption(QStringLiteral("at"),
                                        QStringLiteral("put the cursor on event N"),
                                        QStringLiteral("N")));
    parser.addOption(QCommandLineOption(QStringLiteral("first-diff"),
                                        QStringLiteral("start at the first difference")));
    parser.addOption(QCommandLineOption(QStringLiteral("iters"),
                                        QStringLiteral("main-loop iterations of the recording "
                                                       "(default 6000)"),
                                        QStringLiteral("N"), QStringLiteral("6000")));
    parser.addOption(QCommandLineOption(QStringLiteral("screenshot"),
                                        QStringLiteral("write the window to FILE (PNG) and exit"),
                                        QStringLiteral("FILE")));
    parser.addOption(QCommandLineOption(QStringLiteral("size"),
                                        QStringLiteral("window size for --screenshot"),
                                        QStringLiteral("WxH"), QStringLiteral("1180x720")));
    parser.process(app);

    QTextStream err(stderr);
    const QStringList files = parser.positionalArguments();
    if (files.size() > 2) {
        err << "rewind-gui: at most two traces\n";
        return 2;
    }

    bool ok = false;
    const uint iters = parser.value(QStringLiteral("iters")).toUInt(&ok);
    if (!ok || iters == 0) {
        err << "rewind-gui: --iters wants a positive number\n";
        return 2;
    }

    int width = 1180, height = 720;
    if (parser.isSet(QStringLiteral("size"))) {
        const QStringList wh = parser.value(QStringLiteral("size")).split(QLatin1Char('x'));
        bool okw = false, okh = false;
        if (wh.size() != 2 || (width = wh[0].toInt(&okw), !okw) ||
            (height = wh[1].toInt(&okh), !okh) || width < 400 || height < 300 ||
            width > 8000 || height > 8000) {
            err << "rewind-gui: --size wants WxH, at least 400x300\n";
            return 2;
        }
    }

    MainWindow w;
    w.setMainIters(iters);
    for (int i = 0; i < files.size(); ++i) {
        QString error;
        if (!w.openFile((MainWindow::Side)i, files[i], &error)) {
            err << "rewind-gui: " << error << "\n";
            return 1;
        }
    }

    if (parser.isSet(QStringLiteral("first-diff"))) {
        w.goFirstDifference();
    }
    if (parser.isSet(QStringLiteral("at"))) {
        const qulonglong at = parser.value(QStringLiteral("at")).toULongLong(&ok);
        if (!ok) {
            err << "rewind-gui: --at wants an event number\n";
            return 2;
        }
        w.setCursor(MainWindow::kSideA, (std::size_t)at);
    }

    w.resize(width, height);
    w.show();

    if (parser.isSet(QStringLiteral("screenshot"))) {
        // Let layout and the first paint happen, then take the picture.
        QApplication::processEvents();
        const QImage image = w.grab().toImage();
        const QString out = parser.value(QStringLiteral("screenshot"));
        if (image.isNull() || !image.save(out)) {
            err << "rewind-gui: cannot write " << out << "\n";
            return 1;
        }
        return 0;
    }
    return app.exec();
}
