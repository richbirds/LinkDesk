#include "mainwindow.h"
#include "platform.h"
#include "viewer.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QLabel>
#include <QMenu>
#include <QJsonArray>
#include <QTest>

// Exercise a real event loop and wait for process death, not just window hiding.
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    app.setOrganizationName("LinkDeskTests");
    app.setApplicationName("ShutdownTest");
    QStandardPaths::setTestModeEnabled(true);
    const auto args=app.arguments();
    if(args.contains("--child")) {
        QTemporaryDir state;
        if(!state.isValid()) return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,state.path());
        // Never register on the user's relay or use saved devices/passwords.
        const auto directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QDir().mkpath(directory);
        QFile config(directory+"/relay-config.json");
        if(!config.open(QIODevice::WriteOnly)) return 3;
        config.write("{}");config.close();
        QSettings().setValue("connectionList/devices",QJsonArray{QJsonObject{{"target","123456789"},{"name","UI test"},{"automatic",false}}});
        MainWindow window;window.resize(860,560);
        RemoteClient client;
        ViewerWindow viewer(&client);
        window.show();
        if(args.contains("--viewer")) viewer.begin("Shutdown test",false);
        if(args.contains("--no-auto-quit")) app.setQuitOnLastWindowClosed(false);
        QCoreApplication::processEvents();
        bool footerVisible=false;for(auto label:window.findChildren<QLabel *>()) if(label->text().contains("互联网中继 · v")) {
            const QRect bounds(label->mapTo(&window,QPoint()),label->size());footerVisible=window.rect().contains(bounds);
        }
        if(!footerVisible || window.height()>560) return 10;
        auto preview=window.findChild<QWidget *>("preview-123456789");if(!preview)return 11;
        bool menuWorked=false;
        QTimer::singleShot(100,&app,[&]{auto menu=qobject_cast<QMenu *>(QApplication::activePopupWidget());if(menu){menuWorked=menu->actions().size()==5;menu->close();}});
        QTest::mouseClick(preview,Qt::RightButton);QTest::qWait(150);if(!menuWorked)return 12;
        bool closed=false;
        QTimer::singleShot(300,&window,[&]{closed=window.close();});
        QTimer::singleShot(5000,&app,[&]{app.exit(77);});
        const int result=app.exec();
        QFile::remove(config.fileName());
        return closed ? result : 4;
    }
    for(const auto &mode:QStringList{"plain","viewer","no-auto-quit"}) {
        QProcess child;
        QStringList options{"--child"};
        if(mode!="plain") options<<"--"+mode;
        child.start(QCoreApplication::applicationFilePath(),options);
        if(!child.waitForStarted(5000)) return 5;
        if(!child.waitForFinished(8000)) { child.kill();child.waitForFinished();qCritical("Shutdown timed out: %s",qPrintable(mode));return 6; }
        if(child.exitStatus()!=QProcess::NormalExit || child.exitCode()!=0) { qCritical("Shutdown failed: %s, exit %d",qPrintable(mode),child.exitCode());return 7; }
    }
    return 0;
}
