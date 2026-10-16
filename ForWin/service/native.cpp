#include "native.h"
#include "relay.h"
#include <sddl.h>
#include <aclapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
namespace HostService {
QString winError(const QString &operation,DWORD code){return operation+QString(" (Windows %1)").arg(code);}
static QString folder(int id){wchar_t path[MAX_PATH]{};return SUCCEEDED(SHGetFolderPathW(nullptr,id,nullptr,SHGFP_TYPE_CURRENT,path))?QString::fromWCharArray(path):QString();}
QString installDirectory(){return folder(CSIDL_PROGRAM_FILES)+"/LinkDesk";}
QString dataDirectory(){return folder(CSIDL_COMMON_APPDATA)+"/LinkDesk/Host";}
QString tokenSid(HANDLE token){DWORD n=0;GetTokenInformation(token,TokenUser,nullptr,0,&n);QByteArray data(int(n),'\0');if(!n || !GetTokenInformation(token,TokenUser,data.data(),n,&n))return {};LPWSTR text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid,&text))return {};auto value=QString::fromWCharArray(text);LocalFree(text);return value;}
QString currentSid(){Handle t;HANDLE h=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&h))return {};t.reset(h);return tokenSid(t.value);}
bool elevated(){Handle t;HANDLE h=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&h))return false;t.reset(h);TOKEN_ELEVATION e{};DWORD n=0;return GetTokenInformation(h,TokenElevation,&e,sizeof(e),&n)&&e.TokenIsElevated;}
bool systemProcess(DWORD pid){Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));HANDLE h=nullptr;if(!p || !OpenProcessToken(p.value,TOKEN_QUERY,&h))return false;Handle t(h);return tokenSid(h)=="S-1-5-18";}
bool enablePrivilege(const wchar_t *name){HANDLE h=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&h))return false;Handle t(h);TOKEN_PRIVILEGES p{};p.PrivilegeCount=1;if(!LookupPrivilegeValueW(nullptr,name,&p.Privileges[0].Luid))return false;p.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(ERROR_SUCCESS);return AdjustTokenPrivileges(h,FALSE,&p,0,nullptr,nullptr)&&GetLastError()==ERROR_SUCCESS;}
bool noReparse(const QString &path){auto current=QDir::cleanPath(QFileInfo(path).absoluteFilePath());while(!current.isEmpty()){auto w=QDir::toNativeSeparators(current).toStdWString();const auto a=GetFileAttributesW(w.c_str());if(a!=INVALID_FILE_ATTRIBUTES && (a&FILE_ATTRIBUTE_REPARSE_POINT))return false;const auto parent=QFileInfo(current).dir().absolutePath();if(parent==current)break;current=parent;}return true;}
bool trustedPath(const QString &path,QString *error,bool privateOnly){
    if(!noReparse(path)){if(error)*error="Reparse point in protected path";return false;}
    const auto name=QDir::toNativeSeparators(path).toStdWString();PSID owner=nullptr;PACL acl=nullptr;PSECURITY_DESCRIPTOR sd=nullptr;
    if(GetNamedSecurityInfoW(const_cast<LPWSTR>(name.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&owner,nullptr,&acl,nullptr,&sd)!=ERROR_SUCCESS)return false;
    auto sidText=[](PSID sid){LPWSTR text=nullptr;if(!sid || !ConvertSidToStringSidW(sid,&text))return QString();const auto result=QString::fromWCharArray(text);LocalFree(text);return result;};
    const auto administrator=[](const QString &sid){return sid=="S-1-5-18" || sid=="S-1-5-32-544";};
    bool ok=administrator(sidText(owner)) && acl;
    if(acl)for(DWORD i=0;i<acl->AceCount;++i){LPVOID raw=nullptr;if(!GetAce(acl,i,&raw)){ok=false;break;}
        const auto header=static_cast<ACE_HEADER *>(raw);if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){ok=false;break;}
        auto ace=static_cast<ACCESS_ALLOWED_ACE *>(raw);const auto sid=sidText(&ace->SidStart);
        constexpr DWORD writes=GENERIC_ALL|GENERIC_WRITE|WRITE_DAC|WRITE_OWNER|DELETE|FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|FILE_DELETE_CHILD;
        if(!administrator(sid) && ((ace->Mask&writes) || (privateOnly && ace->Mask)))ok=false;
    }
    if(sd)LocalFree(sd);if(!ok && error)*error="Protected path has an untrusted owner or excessive ACL permissions";return ok;
}
bool protectDirectory(const QString &path,bool executable,QString *error){
    if(QFileInfo::exists(path) && !trustedPath(path,error))return false;
    if(!noReparse(path) || !QDir().mkpath(path)){if(error)*error="Unsafe or inaccessible installation/data directory";return false;}
    const QString sddl=executable ? "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)" : "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&sd,nullptr))return false;
    auto name=QDir::toNativeSeparators(path).toStdWString();const bool ok=SetFileSecurityW(name.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION|OWNER_SECURITY_INFORMATION,sd);LocalFree(sd);
    if(!ok && error)*error=winError("Protect directory");return ok;
}
QByteArray seal(const QByteArray &plain,QString *error){DATA_BLOB in{DWORD(plain.size()),reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))},out{};if(!CryptProtectData(&in,L"LinkDesk machine host",nullptr,nullptr,nullptr,CRYPTPROTECT_LOCAL_MACHINE|CRYPTPROTECT_UI_FORBIDDEN,&out)){if(error)*error=winError("Protect machine credentials");return {}; }QByteArray b(reinterpret_cast<char *>(out.pbData),int(out.cbData));SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);return b;}
QByteArray unseal(const QByteArray &cipher,QString *error){DATA_BLOB in{DWORD(cipher.size()),reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()))},out{};if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out)){if(error)*error=winError("Read machine credentials");return {}; }QByteArray b(reinterpret_cast<char *>(out.pbData),int(out.cbData));SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);return b;}
bool writePrivate(const QString &path,const QByteArray &data,QString *error){if(!noReparse(path)){if(error)*error="Reparse points are not accepted";return false;}QSaveFile f(path);if(!f.open(QIODevice::WriteOnly) || f.write(data)!=data.size() || !f.commit()){if(error)*error="Cannot save protected data";return false;}return true;}
DWORD servicePid(){SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm)return 0;SC_HANDLE svc=OpenServiceW(scm,Name,SERVICE_QUERY_STATUS|SERVICE_QUERY_CONFIG);DWORD result=0;if(svc){SERVICE_STATUS_PROCESS status{};DWORD n=0;QByteArray config(8192,'\0');auto c=reinterpret_cast<QUERY_SERVICE_CONFIGW *>(config.data());if(QueryServiceStatusEx(svc,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE *>(&status),sizeof(status),&n)&&QueryServiceConfigW(svc,c,DWORD(config.size()),&n)){
        const auto expected="\""+QDir::toNativeSeparators(installDirectory()+"/LinkDeskService.exe")+"\"";
        if(QString::fromWCharArray(c->lpBinaryPathName).compare(expected,Qt::CaseInsensitive)==0 && QString::fromWCharArray(c->lpServiceStartName).compare("LocalSystem",Qt::CaseInsensitive)==0)result=status.dwProcessId;
    }CloseServiceHandle(svc);}CloseServiceHandle(scm);return result;}
bool installed(){SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm)return false;SC_HANDLE svc=OpenServiceW(scm,Name,SERVICE_QUERY_STATUS);const bool ok=svc; if(svc)CloseServiceHandle(svc);CloseServiceHandle(scm);return ok;}
QJsonObject Config::json() const{return {{"version",1},{"server",server},{"port",int(port)},{"fingerprint",pin},{"enrollmentKey",enrollment},{"password",password},{"operatorSid",operatorSid},{"unattended",unattended},{"files",files},{"power",power},{"sas",sas},{"sessionMode",sessionMode}};}
bool Config::parse(const QJsonObject &j,Config *c,QString *error){
    const QSet<QString> allowed={"version","server","port","fingerprint","enrollmentKey","password","operatorSid","unattended","files","power","sas","sessionMode"};
    for(auto it=j.begin();it!=j.end();++it)if(!allowed.contains(it.key())){if(error)*error="Unknown machine policy field";return false;}
    if(j.value("version").toInt()!=1 || !j.value("port").isDouble() || j.value("port").toInt()<1 || j.value("port").toInt()>65535){if(error)*error="Invalid machine policy version or port";return false;}
    for(auto k:{"unattended","files","power","sas"})if(!j.value(k).isBool()){if(error)*error="Missing explicit capability authorization";return false;}
    c->server=j.value("server").toString();c->port=quint16(j.value("port").toInt());c->pin=RelayConfig::normalizePin(j.value("fingerprint").toString());c->enrollment=j.value("enrollmentKey").toString();c->password=j.value("password").toString();c->operatorSid=j.value("operatorSid").toString();c->unattended=j.value("unattended").toBool();c->files=j.value("files").toBool();c->power=j.value("power").toBool();c->sas=j.value("sas").toBool();c->sessionMode=j.value("sessionMode").toString();
    RelayConfig relay{c->server,c->pin,c->enrollment,c->port};PSID sid=nullptr;
    const bool valid=relay.valid(error) && c->pin.size()==64 && c->password.size()>=8 && c->password.size()<=64 && !c->password.contains(QChar::Null) && !c->password.contains('\n') && !c->password.contains('\r') && ConvertStringSidToSidW(reinterpret_cast<LPCWSTR>(c->operatorSid.utf16()),&sid) && IsValidSid(sid) && (c->sessionMode=="console" || c->sessionMode=="active");
    if(sid)LocalFree(sid);if(!valid && error && error->isEmpty())*error="Invalid machine policy or operator SID";return valid;
}
bool Config::load(Config *c,QString *error){const auto path=dataDirectory()+"/machine.dat";if(!trustedPath(QFileInfo(dataDirectory()).absolutePath(),error,true) || !trustedPath(dataDirectory(),error,true) || !trustedPath(path,error,true)){if(error)*error="Unsafe machine policy path";return false;}QFile f(path);if(!f.open(QIODevice::ReadOnly) || f.size()>65536){if(error)*error="Machine policy unavailable";return false;}auto plain=unseal(f.readAll(),error);return !plain.isEmpty() && parse(QJsonDocument::fromJson(plain).object(),c,error);}
bool Config::save(QString *error) const {auto encrypted=seal(QJsonDocument(json()).toJson(QJsonDocument::Compact),error);return !encrypted.isEmpty() && writePrivate(dataDirectory()+"/machine.dat",encrypted,error);}
}
