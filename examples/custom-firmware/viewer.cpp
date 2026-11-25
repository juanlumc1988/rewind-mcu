// SPDX-License-Identifier: Apache-2.0
// Part of rewind -- deterministic record & replay for bare-metal firmware.
//
// The viewer, for the thermostat. It is the same window as rewind-gui; the only
// thing a viewer for another firmware has to say is which firmware a trace is
// for, which is the factory. Built only when rewind was installed with the viewer.
//
//   thermostat_viewer a.rwd [b.rwd]

#include <QApplication>

#include "mainwindow.h"
#include "thermostat_firmware.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    MainWindow w;
    w.setMainIters(4000);
    w.setFirmwareFactory(&thermostat::factory);
    for (int i = 1; i < argc && i <= 2; ++i) {
        QString error;
        if (!w.openFile(static_cast<MainWindow::Side>(i - 1), QString::fromLocal8Bit(argv[i]), &error)) {
            qWarning("%s", qPrintable(error));
            return 1;
        }
    }
    w.show();
    return app.exec();
}
