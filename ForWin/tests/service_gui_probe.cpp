#include "mainwindow.h"
#include "native.h"
#include "pipe.h"
#include "platform.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QSettings>
#include <QTimer>
#include <QLabel>
#include <QCheckBox>
// Integration-only ordinary-user UI check. Never elevate this executable.
int main(int argc,char **argv){
    QApplication app(argc,argv);if(HostService::systemProcess(GetCurrentProcessId())||HostService::elevated()||!HostService::installed())return 2;
    app.setApplicationVersion("service integration");app.setOrganizationName("LinkDeskTests");app.setApplicationName("LinkDeskServiceGuiProbe");app.setStyle("Fusion");
    QTemporaryDir state;if(!state.isValid())return 3;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,state.path());
    Platform::initialize();MainWindow window;window.show();QTimer check;check.setInterval(700);
    QObject::connect(&check,&QTimer::timeout,&app,[&]{
        QJsonObject status;QString reason;if(!HostService::controlCall("status",&status,&reason))return;
        auto id=status.value("deviceId").toString();if(id.size()!=9)return;id.insert(6,' ');id.insert(3,' ');
        bool displayed=false;for(auto label:window.findChildren<QLabel *>())if(label->text()==id)displayed=true;
        auto control=window.findChild<QCheckBox *>("allowRemoteControl");auto remember=window.findChild<QCheckBox *>("saveHostPassword");
        if(!displayed||!control||!remember||control->isEnabled()||remember->isEnabled()||control->isChecked()!=status.value("unattended").toBool()||!remember->isChecked())return;
        if(app.arguments().size()==2 && !window.grab().save(app.arguments()[1])){app.exit(4);return;}
        puts("Ordinary-user GUI displays machine identity and administrator-owned controls.");check.stop();app.exit(0);
    });check.start();QTimer::singleShot(15000,&app,[&]{app.exit(5);});return app.exec();
}
