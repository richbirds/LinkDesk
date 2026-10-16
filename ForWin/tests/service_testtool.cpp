#include "native.h"
#include "pipe.h"
#include "platform.h"
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QElapsedTimer>
#include <QThread>
#include <QDir>
#include <wtsapi32.h>
#include <sddl.h>
#include <QUuid>
using namespace HostService;
static int printStatus(){QJsonObject state;QString error;if(!controlCall("status",&state,&error))return 5;const auto b=QJsonDocument(state).toJson(QJsonDocument::Compact);fwrite(b.constData(),1,size_t(b.size()),stdout);fputc('\n',stdout);return 0;}
int main(int argc,char **argv){QCoreApplication app(argc,argv);app.setOrganizationName("LinkDeskTests");app.setApplicationName("ServiceIntegration");const auto args=app.arguments();if(args.size()<2)return 2;
    if(args[1]=="--status")return printStatus();
    if(args[1]=="--sas-test-request"){
        if(!systemProcess(GetCurrentProcessId()))return 59;Config policy;QString error;
        if(!Config::load(&policy,&error)||!policy.unattended)return 60;policy.sas=true;
        const auto bytes=seal(QJsonDocument(QJsonObject{{"policy",policy.json()},{"migration",QJsonObject()}}).toJson(QJsonDocument::Compact),&error);
        if(bytes.isEmpty()||!writePrivate(dataDirectory()+"/sas-validation-request.dat",bytes,&error))return 61;
        puts("Disposable-VM SAS request prepared; no credentials logged.");return 0;
    }

    if(args[1]=="--keys-released"){
        if(systemProcess(GetCurrentProcessId()))return 56;QJsonObject state;QString error;
        if(!controlCall("status",&state,&error)||state.value("desktop").toString()!="Default")return 57;
        for(int key:{VK_SHIFT,VK_CONTROL,VK_MENU,VK_LWIN,VK_RWIN,VK_LBUTTON,VK_RBUTTON,VK_MBUTTON})if(GetAsyncKeyState(key)&0x8000)return 58;
        return 0;
    }

    if(args[1]=="--tokens"){
        if(!systemProcess(GetCurrentProcessId()))return 50;
        QJsonObject state;QString error;if(!controlCall("status",&state,&error))return 51;
        for(auto pid:{servicePid(),DWORD(state.value("workerPid").toDouble())}){
            Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));HANDLE raw=nullptr;
            if(!process||!OpenProcessToken(process.value,TOKEN_QUERY,&raw))return 52;Handle token(raw);DWORD n=0,value=0;
            fprintf(stdout,"Process %lu",pid);
            for(auto kind:{TokenSessionId,TokenUIAccess,TokenElevation,TokenHasRestrictions,TokenIsRestricted})if(GetTokenInformation(raw,kind,&value,sizeof(value),&n))fprintf(stdout," field%d=%lu",int(kind),value);
            QByteArray bytes(8192,'\0');if(GetTokenInformation(raw,TokenIntegrityLevel,bytes.data(),DWORD(bytes.size()),&n)){auto label=reinterpret_cast<TOKEN_MANDATORY_LABEL *>(bytes.data());fprintf(stdout," integrity=%lu",*GetSidSubAuthority(label->Label.Sid,*GetSidSubAuthorityCount(label->Label.Sid)-1));}
            if(GetTokenInformation(raw,TokenPrivileges,bytes.data(),DWORD(bytes.size()),&n)){auto privileges=reinterpret_cast<TOKEN_PRIVILEGES *>(bytes.data());for(DWORD i=0;i<privileges->PrivilegeCount;++i)fprintf(stdout," priv%lu=%lu",privileges->Privileges[i].Luid.LowPart,privileges->Privileges[i].Attributes);}
            fputc('\n',stdout);
        }return 0;
    }

    if(args[1]=="--pause" || args[1]=="--resume"){QJsonObject state;QString error;const bool resume=args[1]=="--resume";if(!controlCall(resume?"resume":"pause",&state,&error))return 42;return state.value("sharing").toBool()==resume?0:43;}
    if((args[1]=="--request" || args[1]=="--promote-request") && args.size()==5){
        QFile server(args[2]);if(!server.open(QIODevice::ReadOnly))return 3;const auto j=QJsonDocument::fromJson(server.readAll()).object();
        Config c;c.server=j.value("server").toString();c.pin=j.value("fingerprint").toString();c.enrollment=j.value("enrollmentKey").toString();c.port=quint16(j.value("port").toInt(443));c.operatorSid=currentSid();c.unattended=true;c.files=true;c.sessionMode="active";
        QByteArray random;for(int n=0;n<8;++n){auto value=QRandomGenerator::system()->generate();random.append(reinterpret_cast<char *>(&value),4);}c.password=QString::fromLatin1(random.toHex());
        QJsonObject migration;
        if(args[1]=="--promote-request"){
            if(systemProcess(GetCurrentProcessId()))return 53;
            // Export under the owning user, never copy user DPAPI blobs into
            // SYSTEM storage. Administrator setup seals fresh machine policy.
            app.setOrganizationName("LinkDesk");app.setApplicationName("LinkDesk");
            c.password=Platform::loadPassword("host-password");
            const auto digest=QCryptographicHash::hash((c.server.toLower()+":"+QString::number(c.port)).toUtf8(),QCryptographicHash::Sha256).toHex().left(16);
            QFile identity(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/devices/"+QString::fromLatin1(digest)+".json");
            if(!identity.open(QIODevice::ReadOnly)||identity.size()>4096)return 54;
            const auto old=QJsonDocument::fromJson(identity.readAll()).object();
            if(!QRegularExpression("^[0-9]{9}$").match(old.value("id").toString()).hasMatch()||!QRegularExpression("^[0-9a-f]{64}$").match(old.value("token").toString()).hasMatch())return 55;
            migration={{"id",old.value("id")},{"token",old.value("token")},{"server",c.server},{"port",int(c.port)},{"fingerprint",c.pin}};
            app.setOrganizationName("LinkDeskTests");app.setApplicationName("ServiceIntegration");
        }
        QString error;Config validated;if(!Config::parse(c.json(),&validated,&error))return 4;
        if(!QDir().mkpath(QFileInfo(args[3]).absolutePath()))return 6;
        const QString sddl="D:P(A;;FA;;;"+c.operatorSid+")(A;;FA;;;SY)(A;;FA;;;BA)";
        auto bytes=seal(QJsonDocument(QJsonObject{{"policy",c.json()},{"migration",migration}}).toJson(QJsonDocument::Compact),&error);
        if(bytes.isEmpty()||!writePrivate(args[3],bytes,&error))return 7;PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&sd,nullptr))return 8;
        auto path=QDir::toNativeSeparators(args[3]).toStdWString();const bool secured=SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd);LocalFree(sd);if(!secured)return 9;
        QFile descriptor(args[4]);if(!descriptor.open(QIODevice::WriteOnly))return 10;auto description=c.json();if(!migration.isEmpty())description["id"]=migration.value("id");descriptor.write(QJsonDocument(description).toJson(QJsonDocument::Compact));descriptor.close();
        sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&sd,nullptr))return 8;
        path=QDir::toNativeSeparators(args[4]).toStdWString();const bool privateDescriptor=SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd);LocalFree(sd);if(!privateDescriptor)return 9;
        puts("Private integration request prepared; no credentials logged.");return 0;
    }
    if(args[1]=="--data-denied"){
        const auto path=QDir::toNativeSeparators(dataDirectory()+"/machine.dat").toStdWString();for(DWORD access:{DWORD(GENERIC_READ),DWORD(GENERIC_WRITE)}){Handle file(CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr));if(file || GetLastError()!=ERROR_ACCESS_DENIED)return 11;}return 0;
    }
    if(args[1]=="--private-acl"){
        if(!systemProcess(GetCurrentProcessId()))return 38;enablePrivilege(SE_RESTORE_NAME);
        const auto path=dataDirectory()+"/acl-probe-"+QUuid::createUuid().toString(QUuid::Id128)+".tmp";QString error;if(!writePrivate(path,"Synthetic ACL fixture; no credentials",&error))return 39;
        PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;BU)",SDDL_REVISION_1,&sd,nullptr)){QFile::remove(path);return 40;}
        auto native=QDir::toNativeSeparators(path).toStdWString();const bool set=SetFileSecurityW(native.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION|OWNER_SECURITY_INFORMATION,sd);LocalFree(sd);
        const bool rejected=set && trustedPath(path,&error) && !trustedPath(path,&error,true);QFile::remove(path);return rejected?0:41;
    }
    if(args[1]=="--unknown-ipc"){
        auto handle=clientPipe("LinkDeskHost-control-v1",servicePid());if(!handle)return 12;
        std::atomic<bool> replied{false};PipeChannel pipe(std::move(handle),false,{},[&](Kind,const QByteArray &){replied=true;},[]{});
        pipe.send(Kind::Control,"{\"version\":1,\"command\":\"execute\",\"path\":\"cmd.exe\"}");QThread::msleep(500);pipe.stop();return replied?13:0;
    }
    if(args[1]=="--crash-worker" || args[1]=="--crash-broker"){
        const bool capture=args[1]=="--crash-worker";const auto field=capture?"workerPid":"userBrokerPid";
        QJsonObject before;QString error;if(!controlCall("status",&before,&error))return 14;const DWORD pid=DWORD(before.value(field).toDouble());Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE|SYNCHRONIZE,FALSE,pid));if(!process||systemProcess(pid)!=capture)return 15;
        WCHAR name[32768]{};DWORD n=32768;if(!QueryFullProcessImageNameW(process.value,0,name,&n)||QString::fromWCharArray(name).compare(QDir::toNativeSeparators(installDirectory()+"/LinkDeskWorker.exe"),Qt::CaseInsensitive)!=0)return 16;
        if(!TerminateProcess(process.value,73)||WaitForSingleObject(process.value,3000)!=WAIT_OBJECT_0)return 17;QElapsedTimer time;time.start();
        while(time.elapsed()<12000){QThread::msleep(300);QJsonObject now;if(controlCall("status",&now,&error)&&now.value(field).toDouble()>0&&DWORD(now.value(field).toDouble())!=pid&&now.value("deviceId")==before.value("deviceId"))return 0;}return 18;
    }
    if(args[1]=="--stop-clean"){
        QJsonObject state;QString error;if(!controlCall("status",&state,&error))return 26;
        Handle host(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,servicePid()));
        Handle capture(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(state.value("workerPid").toDouble())));
        Handle broker(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(state.value("userBrokerPid").toDouble())));
        SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!manager || !host)return 27;
        SC_HANDLE service=OpenServiceW(manager,Name,SERVICE_STOP);SERVICE_STATUS status{};const bool sent=service && ControlService(service,SERVICE_CONTROL_STOP,&status);if(service)CloseServiceHandle(service);CloseServiceHandle(manager);
        if(!sent || WaitForSingleObject(host.value,15000)!=WAIT_OBJECT_0)return 28;
        DWORD code=0;if(!GetExitCodeProcess(host.value,&code)||code!=0){fprintf(stderr,"Stopped service process exit: %lu\n",static_cast<unsigned long>(code));return 29;}
        for(auto child:{capture.value,broker.value})if(child && WaitForSingleObject(child,2000)!=WAIT_OBJECT_0)return 30;
        puts("Service exited cleanly; captured worker handles are signaled.");return 0;
    }
    if(args[1]=="--roles"){
        QJsonObject state;QString error;if(!controlCall("status",&state,&error))return 31;
        const DWORD selected=DWORD(state.value("session").toDouble());DWORD hostSession=1,captureSession=0,brokerSession=0;
        const DWORD host=servicePid(),capture=DWORD(state.value("workerPid").toDouble()),broker=DWORD(state.value("userBrokerPid").toDouble());
        if(!ProcessIdToSessionId(host,&hostSession)||hostSession!=0||!systemProcess(host)||!ProcessIdToSessionId(capture,&captureSession)||captureSession!=selected||!systemProcess(capture))return 32;
        if(broker){if(!ProcessIdToSessionId(broker,&brokerSession)||brokerSession!=selected||systemProcess(broker))return 33;HANDLE raw=nullptr;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,broker));if(!process||!OpenProcessToken(process.value,TOKEN_QUERY,&raw))return 34;Handle token(raw);DWORD n=0;TOKEN_ELEVATION elevation{};if(!GetTokenInformation(raw,TokenElevation,&elevation,sizeof(elevation),&n)||elevation.TokenIsElevated)return 35;
            GetTokenInformation(raw,TokenIntegrityLevel,nullptr,0,&n);QByteArray data(int(n),'\0');if(!GetTokenInformation(raw,TokenIntegrityLevel,data.data(),n,&n))return 36;auto label=reinterpret_cast<TOKEN_MANDATORY_LABEL *>(data.data());const DWORD integrity=*GetSidSubAuthority(label->Label.Sid,*GetSidSubAuthorityCount(label->Label.Sid)-1);if(integrity>SECURITY_MANDATORY_MEDIUM_RID)return 37;
        }puts(broker?"Roles verified: Session 0 service, selected-session SYSTEM capture, medium-integrity ordinary-user file broker.":"Roles verified: Session 0 service and selected-session SYSTEM capture; no user broker active.");return 0;
    }
    if(args[1]=="--restricted-ipc"){
        HANDLE original=nullptr;if(!WTSQueryUserToken(WTSGetActiveConsoleSessionId(),&original))return 19;Handle user(original);BYTE users[SECURITY_MAX_SID_SIZE]{};DWORD length=sizeof(users);if(!CreateWellKnownSid(WinBuiltinUsersSid,nullptr,users,&length))return 20;BYTE authenticated[SECURITY_MAX_SID_SIZE]{},everyone[SECURITY_MAX_SID_SIZE]{};DWORD au=sizeof(authenticated),world=sizeof(everyone);if(!CreateWellKnownSid(WinAuthenticatedUserSid,nullptr,authenticated,&au)||!CreateWellKnownSid(WinWorldSid,nullptr,everyone,&world))return 20;SID_AND_ATTRIBUTES restriction[]{{users,0},{authenticated,0},{everyone,0}};HANDLE token=nullptr;
        if(!CreateRestrictedToken(user.value,DISABLE_MAX_PRIVILEGE,0,nullptr,0,nullptr,3,restriction,&token))return 21;Handle restricted(token);
        if(!ImpersonateLoggedOnUser(restricted.value))return 22;
        Handle pipe(CreateFileW(L"\\\\.\\pipe\\LinkDeskHost-control-v1",FILE_READ_DATA|FILE_WRITE_DATA|READ_CONTROL|SYNCHRONIZE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr));
        const auto code=GetLastError();RevertToSelf();
        return !pipe && code==ERROR_ACCESS_DENIED?0:25;
    }
    return 2;
}
