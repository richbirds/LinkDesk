#include "systemsettings.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QVariant>
#include <QSaveFile>
#include <QSettings>
#include <QProcess>
#ifdef Q_OS_MACOS
static QString agentPath() {return QDir::homePath()+"/Library/LaunchAgents/com.linkdesk.client.plist";}
#endif
bool SystemSettings::autoStartEnabled() {
#ifdef Q_OS_WIN
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",QSettings::NativeFormat);return run.contains("LinkDesk");
#else
    return QFile::exists(agentPath());
#endif
}
bool SystemSettings::setAutoStart(bool enabled,QString *error) {
    if(QCoreApplication::instance()->property("linkdesk.previewMode").toBool()) {if(error)*error="预览模式不能修改开机启动";return false;}
#ifdef Q_OS_WIN
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",QSettings::NativeFormat);
    if(enabled) run.setValue("LinkDesk","\""+QDir::toNativeSeparators(QCoreApplication::applicationFilePath())+"\"");else run.remove("LinkDesk");run.sync();
    if(run.status()==QSettings::NoError)return true;
#else
    if(!enabled) {if(!QFile::exists(agentPath()) || QFile::remove(agentPath())) return true;}
    else {
        QDir().mkpath(QFileInfo(agentPath()).absolutePath());QSaveFile file(agentPath());
        const QString xml="<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict><key>Label</key><string>com.linkdesk.client</string><key>ProgramArguments</key><array><string>"+QCoreApplication::applicationFilePath().toHtmlEscaped()+"</string></array><key>RunAtLoad</key><true/></dict></plist>";
        if(file.open(QIODevice::WriteOnly) && file.write(xml.toUtf8())==xml.toUtf8().size() && file.commit()) return true;
    }
#endif
    if(error)*error="无法保存开机启动配置";return false;
}
bool SystemSettings::power(const QString &action,QString *error) {
    if(action!="restart" && action!="shutdown") {if(error)*error="无效的电源操作";return false;}
#ifdef Q_OS_WIN
    const bool ok=QProcess::startDetached("shutdown.exe",{action=="restart" ? "/r" : "/s","/t","30"});
#else
    const bool ok=QProcess::startDetached("/usr/bin/osascript",{"-e",action=="restart" ? "tell application \"System Events\" to restart" : "tell application \"System Events\" to shut down"});
#endif
    if(!ok && error)*error="无法执行电源操作";return ok;
}
