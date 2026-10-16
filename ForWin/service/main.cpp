#include "native.h"
#include "backend.h"
#include "remote.h"
#include "relay.h"
#include "platform.h"
#include "exitdeadline.h"
#include <wtsapi32.h>
#include <QCoreApplication>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>
#include <atomic>
#include <mutex>
#include <vector>
#include <condition_variable>
#include <thread>
using namespace HostService;
namespace {
SERVICE_STATUS_HANDLE statusHandle=nullptr;
std::atomic<bool> stopping{false};std::mutex eventMutex;
std::vector<std::pair<DWORD,DWORD>> sessionEvents;
void status(DWORD state,DWORD code=NO_ERROR){SERVICE_STATUS s{};s.dwServiceType=SERVICE_WIN32_OWN_PROCESS;s.dwCurrentState=state;s.dwWin32ExitCode=code;s.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|SERVICE_ACCEPT_SESSIONCHANGE:0;s.dwWaitHint=state==SERVICE_STOP_PENDING?15000:state==SERVICE_START_PENDING?15000:0;if(statusHandle)SetServiceStatus(statusHandle,&s);}
DWORD WINAPI control(DWORD code,DWORD event,LPVOID data,LPVOID){
    if(code==SERVICE_CONTROL_STOP || code==SERVICE_CONTROL_SHUTDOWN){stopping=true;status(SERVICE_STOP_PENDING);return NO_ERROR;}
    if(code==SERVICE_CONTROL_SESSIONCHANGE && data){auto notification=static_cast<WTSSESSION_NOTIFICATION *>(data);std::lock_guard<std::mutex> lock(eventMutex);if(sessionEvents.size()<64)sessionEvents.emplace_back(event,notification->dwSessionId);return NO_ERROR;}
    if(code==SERVICE_CONTROL_INTERROGATE)return NO_ERROR;return ERROR_CALL_NOT_IMPLEMENTED;
}
DWORD runHost(QCoreApplication &app){
    if(!systemProcess(GetCurrentProcessId())){return ERROR_ACCESS_DENIED;}
    for(auto privilege:{SE_TCB_NAME,SE_ASSIGNPRIMARYTOKEN_NAME,SE_INCREASE_QUOTA_NAME,SE_IMPERSONATE_NAME})enablePrivilege(privilege);
    SetDllDirectoryW(L"");
    Config config;QString error;
    if(!trustedPath(installDirectory(),&error)||!trustedPath(installDirectory()+"/LinkDeskService.exe",&error))return ERROR_ACCESS_DENIED;
    if(QFile::exists(dataDirectory()+"/installation-incomplete"))return ERROR_INSTALL_FAILURE;
    if(!Config::load(&config,&error)){return ERROR_INVALID_DATA;}
    for(auto file:{"identity.pem","identity.key","machine-device.json"}){const auto path=dataDirectory()+"/"+file;if(QFileInfo::exists(path) && !trustedPath(path,&error,true)){return ERROR_ACCESS_DENIED;}}
    QCoreApplication::setLibraryPaths({installDirectory()});
    Platform::initialize();
    {
        Backend backend(config);RemoteHost host;RelayAgent relay;host.setBackend(&backend);host.setStorageDirectory(dataDirectory());
        if(!host.setCode(config.password,&error)){return ERROR_INVALID_DATA;}
        host.setControl(config.unattended,&error);
        bool sharing=false,online=false,connected=false,paused=false;QString id;
        const auto sharingPath=dataDirectory()+"/sharing.json";
        if(QFile::exists(sharingPath)){QFile file(sharingPath);if(!trustedPath(sharingPath,&error,true)||!file.open(QIODevice::ReadOnly)||file.size()>128)return ERROR_INVALID_DATA;const auto saved=QJsonDocument::fromJson(file.readAll()).object();if(saved.size()!=1||!saved.value("paused").isBool())return ERROR_INVALID_DATA;paused=saved.value("paused").toBool();}
        auto update=[&]{backend.setHostStatus(online,id,host.fingerprint(),sharing,connected);};
        auto share=[&](bool enable){if(enable && config.unattended){if(!host.isListening())sharing=host.start(45432,&error,false,QHostAddress::Any)||host.start(0,&error,false,QHostAddress::Any);if(sharing){backend.setHostError({});relay.setAvailable(host.port(),host.fingerprint());}else backend.setHostError(error);}
            else {sharing=false;relay.setAvailable(0);host.stop();backend.activate(false);}update();};
        backend.control=[&](const QString &command){
            if(command=="pause" || command=="resume"){const bool next=command=="pause";if(!writePrivate(sharingPath,QJsonDocument(QJsonObject{{"paused",next}}).toJson(QJsonDocument::Compact),&error)){backend.setHostError(error);update();return;}paused=next;backend.setHostError({});share(!paused);}
            else if(command=="disconnect")host.disconnectPeer();
        };
        QObject::connect(&relay,&RelayAgent::onlineChanged,&app,[&](bool value,const QString &device){online=value;id=device;update();});
        QObject::connect(&host,&RemoteHost::peerChanged,&app,[&](bool value,const QString &){connected=value;update();});
        share(!paused);
        RelayConfig server{config.server,config.pin,config.enrollment,config.port};
        CredentialMigration migration;
        const auto journalPath=dataDirectory()+"/migration.json";
        QJsonObject journal;
        if(QFileInfo::exists(journalPath)){
            QFile file(journalPath);
            if(!trustedPath(journalPath,&error,true)||!file.open(QIODevice::ReadOnly)||file.size()>8192){backend.shutdown();return ERROR_INVALID_DATA;}
            journal=QJsonDocument::fromJson(file.readAll()).object();
            const QRegularExpression token("^[0-9a-f]{64}$"),device("^[0-9]{9}$");
            if(journal.size()!=7 || journal.value("version").toInt()!=1 || journal.value("server").toString().compare(config.server,Qt::CaseInsensitive)!=0 || journal.value("port").toInt()!=config.port || journal.value("fingerprint").toString()!=config.pin || !device.match(journal.value("id").toString()).hasMatch() || !token.match(journal.value("oldToken").toString()).hasMatch() || !token.match(journal.value("newToken").toString()).hasMatch() || journal.value("oldToken")==journal.value("newToken")){
                backend.shutdown();return ERROR_INVALID_DATA;
            }
        }
        QTimer finalize;finalize.setInterval(5000);
        auto finishMigration=[&]{
            const QJsonObject identity{{"id",journal.value("id")},{"token",journal.value("newToken")}};
            if(!writePrivate(dataDirectory()+"/machine-device.json",QJsonDocument(identity).toJson(QJsonDocument::Compact),&error))return;
            if(!QFile::remove(journalPath))return;
            finalize.stop();backend.setMigrationPending(false);relay.start(server,dataDirectory()+"/machine-device.json");
        };
        QObject::connect(&finalize,&QTimer::timeout,&app,finishMigration);
        QObject::connect(&migration,&CredentialMigration::completed,&app,[&]{finalize.start();finishMigration();});
        if(journal.isEmpty())relay.start(server,dataDirectory()+"/machine-device.json");
        else {backend.setMigrationPending(true);id=journal.value("id").toString();update();migration.start(server,id,journal.value("oldToken").toString(),journal.value("newToken").toString());}
        QTimer watch;watch.setInterval(50);QObject::connect(&watch,&QTimer::timeout,&app,[&]{
            std::vector<std::pair<DWORD,DWORD>> events;{std::lock_guard<std::mutex> lock(eventMutex);events.swap(sessionEvents);}
            for(const auto &event:events)backend.sessionEvent(event.first,event.second);
            if(stopping){watch.stop();migration.stop();finalize.stop();startExitDeadline();host.stop();relay.stop();backend.shutdown();app.quit();}
        });watch.start();status(SERVICE_RUNNING);app.exec();host.stop();relay.stop();backend.shutdown();
    }
    return NO_ERROR;
}
// Qt owns the process main thread. The SCM callback thread contains no Qt
// objects or adopted-thread TLS; it only reports state and waits for teardown.
std::mutex startMutex;std::condition_variable startCondition;bool dispatcherReady=false;
DWORD dispatcherError=NO_ERROR,hostError=NO_ERROR;HANDLE hostDone=nullptr;
void WINAPI serviceMain(DWORD,LPWSTR *){
    statusHandle=RegisterServiceCtrlHandlerExW(Name,control,nullptr);
    {std::lock_guard<std::mutex> lock(startMutex);if(!statusHandle)dispatcherError=GetLastError();else status(SERVICE_START_PENDING);dispatcherReady=true;}startCondition.notify_one();
    if(!statusHandle)return;
    WaitForSingleObject(hostDone,INFINITE);status(SERVICE_STOPPED,hostError);
}
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);app.setOrganizationName("LinkDesk");app.setApplicationName("LinkDeskMachineHost");
    Handle done(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!done)return int(GetLastError());hostDone=done.value;
    std::thread dispatcher([]{SERVICE_TABLE_ENTRYW table[]{{const_cast<LPWSTR>(Name),serviceMain},{nullptr,nullptr}};
        if(!StartServiceCtrlDispatcherW(table)){std::lock_guard<std::mutex> lock(startMutex);dispatcherError=GetLastError();dispatcherReady=true;startCondition.notify_one();}
    });
    {std::unique_lock<std::mutex> lock(startMutex);startCondition.wait(lock,[]{return dispatcherReady;});}
    if(!dispatcherError)hostError=runHost(app);
    SetEvent(hostDone);dispatcher.join();return int(dispatcherError?dispatcherError:hostError);
}
