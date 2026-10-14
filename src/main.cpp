#include "mainwindow.h"
#include "platform.h"
#include <QApplication>
#include <QDir>
#include <QTimer>
#include <QImageReader>
#include <QIcon>
int main(int argc,char **argv) {
    QApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QApplication app(argc,argv);
    app.setStyle("Fusion");
    app.setWindowIcon(QIcon(":/brand/linkdesk.png"));
    app.setOrganizationName("LinkDesk"); app.setApplicationName("LinkDesk"); app.setApplicationVersion("0.7.0");
    QImageReader::setAllocationLimit(64);
    Platform::initialize();
    MainWindow window; window.show();
    const auto args=app.arguments();
    int index=args.indexOf("--screenshot");
    if (index>=0 && index+1<args.size()) {
        const auto path=args[index+1];
        QTimer::singleShot(1200,&window,[&app,&window,path] { app.exit(window.grab().save(path) ? 0 : 1); });
    }
    return app.exec();
}
