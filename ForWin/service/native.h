#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <QString>
#include <QJsonObject>
#include <QByteArray>
namespace HostService {
inline constexpr wchar_t Name[]=L"LinkDeskHost";
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    ~Handle(){reset();} Handle(const Handle &)=delete;Handle &operator=(const Handle &)=delete;
    Handle(Handle &&h) noexcept:value(h.value){h.value=nullptr;}
    Handle &operator=(Handle &&h) noexcept{if(this!=&h){reset();value=h.value;h.value=nullptr;}return *this;}
    void reset(HANDLE h=nullptr){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=h;}
    explicit operator bool() const{return value && value!=INVALID_HANDLE_VALUE;}
};
QString winError(const QString &operation,DWORD code=GetLastError());
QString installDirectory();QString dataDirectory();QString currentSid();QString tokenSid(HANDLE token);
bool systemProcess(DWORD pid);bool elevated();bool enablePrivilege(const wchar_t *name);
bool trustedPath(const QString &path,QString *error,bool privateOnly=false);
bool noReparse(const QString &path);bool protectDirectory(const QString &path,bool executable,QString *error);
QByteArray seal(const QByteArray &plain,QString *error);QByteArray unseal(const QByteArray &cipher,QString *error);
bool writePrivate(const QString &path,const QByteArray &data,QString *error);
DWORD servicePid();bool installed();QString queryStatus(QString *error=nullptr);
struct Config {
    QString server,pin,enrollment,password,operatorSid;
    quint16 port=443;
    bool unattended=false,files=false,power=false,sas=false;
    QString sessionMode="console";
    QJsonObject json() const;
    static bool parse(const QJsonObject &,Config *,QString *error);
    static bool load(Config *,QString *error);
    bool save(QString *error) const;
};
}
