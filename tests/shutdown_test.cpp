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
#include <QPushButton>
#include <QMessageBox>
#include <QStackedWidget>
#include <thread>
#include <chrono>

static void answer(QMessageBox::StandardButton choice) {
    QTimer::singleShot(50,qApp,[choice]{
        auto box=qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if(box && box->objectName()=="exitConfirmation") box->button(choice)->click();
    });
}
// Wait for process death with an authenticated peer and outstanding work. A
// hidden window or the watchdog fallback is not a clean shutdown result.
int main(int argc,char **argv) {
    QApplication app(argc,argv);app.setOrganizationName("LinkDeskTests");app.setApplicationName("ShutdownTest");QStandardPaths::setTestModeEnabled(true);
    const auto args=app.arguments();
    if(args.contains("--child")) {
        QTemporaryDir state;if(!state.isValid())return 2;
        QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,state.path());
        const auto directory=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);QDir().mkpath(directory);
        QFile config(directory+"/relay-config.json");if(!config.open(QIODevice::WriteOnly))return 3;config.write("{}");config.close();
        QSettings().setValue("connectionList/devices",QJsonArray{QJsonObject{{"target","123456789"},{"name","UI test"},{"automatic",false}}});
        MainWindow window;window.resize(860,560);window.show();
        auto host=window.findChild<RemoteHost *>();if(!host)return 14;QString error;if(!host->start(0,&error,true))return 15;
        RemoteClient client;ViewerWindow viewer(&client,&window);
        client.connectTo("127.0.0.1",host->port(),host->code(),"shutdown-test",host->fingerprint());
        if(!QTest::qWaitFor([&]{return client.active();},5000))return 16;
        if(args.contains("--viewer"))viewer.begin("Shutdown test",false);
        QCoreApplication::processEvents();
        bool footerVisible=false;for(auto label:window.findChildren<QLabel *>())if(label->text().contains("互联网中继 · v"))footerVisible=window.rect().contains(QRect(label->mapTo(&window,QPoint()),label->size()));
        if(!footerVisible || window.height()>560)return 10;
        auto preview=window.findChild<QWidget *>("preview-123456789");if(!preview)return 11;
        bool menuWorked=false;QTimer::singleShot(100,&app,[&]{auto menu=qobject_cast<QMenu *>(QApplication::activePopupWidget());if(menu){menuWorked=menu->actions().size()==5;menu->close();}});
        QTest::mouseClick(preview,Qt::RightButton);QTest::qWait(150);if(!menuWorked)return 12;
        if(args.contains("--plain")) {
            auto cardClient=window.findChild<RemoteClient *>();if(!cardClient)return 21;
            emit cardClient->fileProgress("rate-test.dat",0,1024*1024,"传送中");
            emit cardClient->fileProgress("rate-test.dat",65536,1024*1024,"传送中");QTest::qWait(1200);
            auto label=window.findChild<QLabel *>("fileTransferStatus");
            if(!label || !label->text().contains("KB/s") || label->text().contains("0.0 KB/s"))return 22;
            emit cardClient->fileProgress("rate-test.dat",65536,1024*1024,"等待接收 / 自动续传");
            if(label->text().contains("KB/s"))return 23;
        }
        if(args.contains("--transfer")) {
            QFile source(state.filePath("hash-in-progress.dat"));if(!source.open(QIODevice::WriteOnly) || !source.resize(512*1024*1024))return 17;source.close();
            auto transfer=new FileTransfer(&window);transfer->enqueue({source.fileName()});
        }
        auto pages=window.findChild<QStackedWidget *>();pages->setCurrentIndex(1);const auto geometry=window.geometry();
        bool checked=false;
        QTimer::singleShot(100,&window,[&]{
            if(window.close() || window.isVisible() || !client.active()){app.exit(18);return;}
            window.findChild<QAction *>("trayShowAction")->trigger();
            if(!window.isVisible() || window.geometry()!=geometry || pages->currentIndex()!=1 || !client.active()){app.exit(19);return;}
            window.close();answer(QMessageBox::No);window.findChild<QAction *>("trayExitAction")->trigger();
            if(!client.active() || !host->isListening()){app.exit(20);return;}
            window.restoreFromTray();checked=true;
            answer(QMessageBox::Yes);window.findChild<QPushButton *>("completeExitButton")->click();
        });
        if(args.contains("--blocked"))QObject::connect(&app,&QCoreApplication::aboutToQuit,&app,[]{std::this_thread::sleep_for(std::chrono::seconds(20));});
        QTimer::singleShot(15000,&app,[&]{app.exit(77);});
        const int result=app.exec();QFile::remove(config.fileName());return checked ? result : 4;
    }
    for(const auto &mode:QStringList{"plain","viewer","transfer","blocked"}) {
        QProcess child;QStringList options{"--child","--"+mode};child.start(QCoreApplication::applicationFilePath(),options);
        if(!child.waitForStarted(5000))return 5;
        if(!child.waitForFinished(12000)){child.kill();child.waitForFinished();qCritical("Exit timed out: %s",qPrintable(mode));return 6;}
        const int expected=mode=="blocked" ? 70 : 0;
        if(child.exitStatus()!=QProcess::NormalExit || child.exitCode()!=expected){qCritical("Exit failed: %s, code %d; %s",qPrintable(mode),child.exitCode(),child.readAllStandardError().constData());return 7;}
    }
    return 0;
}
