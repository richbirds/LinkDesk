#include "mainwindow.h"
#include "platform.h"
#include "singleinstance.h"
#include <QApplication>
#include <QDir>
#include <QTimer>
#include <QImageReader>
#include <QIcon>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
int main(int argc,char **argv) {
    QApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QApplication app(argc,argv);
    app.setStyle("Fusion");
    app.setWindowIcon(QIcon(":/brand/linkdesk.png"));
    app.setOrganizationName("LinkDesk"); app.setApplicationName("LinkDesk"); app.setApplicationVersion("0.9.0");
    const auto args=app.arguments();
    const int index=args.indexOf("--screenshot");
    const bool preview=index>=0 && index+1<args.size();
    QTemporaryDir previewState;
    if(preview) {
        if(!previewState.isValid()) return 1;
        app.setApplicationName("LinkDeskPreview");
        app.setProperty("linkdesk.previewMode",true);
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,previewState.path());
    }
    MainWindow *primary=nullptr;
    SingleInstance instance(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    if(!preview) {
        QString error;
        const auto result=instance.acquire([&] {
            if(!primary) return;
            primary->restoreFromTray();
        },&error);
        if(result==SingleInstance::AlreadyRunning) return 0;
        if(result==SingleInstance::Failed) { QMessageBox::critical(nullptr,"LinkDesk 启动失败",error);return 1; }
    }
    QImageReader::setAllocationLimit(64);
    Platform::initialize();
    MainWindow window; primary=&window;window.show();
    if (preview) {
        const auto path=args[index+1];
        QTimer::singleShot(1200,&window,[&app,&window,path] { app.exit(window.grab().save(path) ? 0 : 1); });
    }
    return app.exec();
}
