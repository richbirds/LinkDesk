#include "gui.h"
#include <shellapi.h>
#include <sddl.h>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>
namespace HostService {
static bool elevate(const QString &arguments,const QString &cleanup,QString *error){
    const auto path=QCoreApplication::applicationDirPath()+"/LinkDeskServiceSetup.exe";
    if(!QFile::exists(path)||!noReparse(path)){if(error)*error="服务安装组件缺失，请使用包含服务工具的完整 Windows 安装包";return false;}
    const auto exe=QDir::toNativeSeparators(path).toStdWString(),args=arguments.toStdWString();SHELLEXECUTEINFOW info{};info.cbSize=sizeof(info);info.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC;info.lpVerb=L"runas";info.lpFile=exe.c_str();info.lpParameters=args.c_str();info.nShow=SW_SHOWNORMAL;
    if(!ShellExecuteExW(&info)){if(error)*error=winError("管理员服务设置已取消或无法启动");return false;}
    auto timer=new QTimer(QCoreApplication::instance());timer->setInterval(500);QObject::connect(timer,&QTimer::timeout,timer,[timer,process=info.hProcess,cleanup]{if(WaitForSingleObject(process,0)!=WAIT_OBJECT_0)return;CloseHandle(process);if(!cleanup.isEmpty())QFile::remove(cleanup);timer->stop();timer->deleteLater();});timer->start();return true;
}
bool launchSetup(const Config &config,const QJsonObject &migration,bool reconfigure,QString *error){
    if(reconfigure)return elevate("--configure",{},error);
    const auto sid=currentSid();if(sid.isEmpty()||sid!=config.operatorSid){if(error)*error="无法确认当前操作用户身份";return false;}
    const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/service-requests";if(!noReparse(dir)||!QDir().mkpath(dir))return false;
    const QString sddl="O:"+sid+"D:P(A;OICI;FA;;;"+sid+")(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)";PSECURITY_DESCRIPTOR sd=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&sd,nullptr))return false;
    const auto pathW=QDir::toNativeSeparators(dir).toStdWString();const bool protectedPath=SetFileSecurityW(pathW.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd);LocalFree(sd);if(!protectedPath)return false;
    const auto request=dir+"/"+QUuid::createUuid().toString(QUuid::Id128)+".dat";const auto bytes=seal(QJsonDocument(QJsonObject{{"policy",config.json()},{"migration",migration}}).toJson(QJsonDocument::Compact),error);
    if(bytes.isEmpty()||!writePrivate(request,bytes,error))return false;
    if(!elevate("--request \""+QDir::toNativeSeparators(request)+"\"",request,error)){QFile::remove(request);return false;}return true;
}
bool launchRemoval(QString *error){return elevate("--remove",{},error);}
}
