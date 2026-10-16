#include "backend.h"
#include "input.h"
#include <cmath>
#include <wtsapi32.h>
#include <userenv.h>
#include <sddl.h>
#include <QDir>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QBuffer>
#include <QImageReader>
#include <QUuid>
#include <QtEndian>
#include <vector>
#include <tlhelp32.h>
namespace HostService {
// One bounded mailbox per worker: reader threads never mutate supervisor state.
// Replacing pending images prevents GUI/event-loop stalls from queuing raw frames.
struct WorkerInbox {std::mutex mutex;std::deque<std::pair<Kind,QByteArray>> queue;bool scheduled=false,overflow=false;};
static QByteArray json(const QJsonObject &j){return QJsonDocument(j).toJson(QJsonDocument::Compact);}
static bool allowedFile(const QJsonObject &j){static const QSet<QString> allowed={"file_offer","file_ready","file_ack","file_done","file_error","file_finish"};return allowed.contains(j.value("type").toString());}
Backend::Backend(const Config &config,QObject *parent):HostBackend(parent),m_config(config){m_frame.pending=true;m_frame.waitReason="等待交互会话工作进程";m_clock.start();m_watch.setInterval(250);connect(&m_watch,&QTimer::timeout,this,&Backend::supervise);m_watch.start();}
Backend::~Backend(){shutdown();}
void Backend::stopChild(std::unique_ptr<Child> &child){if(!child)return;if(child.get()==m_worker.get())++m_workerGeneration;if(child.get()==m_user.get())m_userReady=false;if(child->pipe)child->pipe->stop();if(child->process)WaitForSingleObject(child->process.value,1000);child->job.reset();if(child->process)WaitForSingleObject(child->process.value,2000);child.reset();}
void Backend::shutdown(){if(m_stopping)return;m_stopping=true;m_watch.stop();releaseInput();if(m_control)m_control->stop();stopChild(m_user);stopChild(m_worker);m_control.reset();}
DWORD Backend::selectSession() const {
    const DWORD console=WTSGetActiveConsoleSessionId();
    if(m_config.sessionMode=="console")return console;
    PWTS_SESSION_INFOW sessions=nullptr;DWORD count=0,result=0xffffffff;
    if(WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&sessions,&count)){
        for(DWORD i=0;i<count;++i)if(sessions[i].SessionId && sessions[i].State==WTSActive){if(sessions[i].SessionId==console){result=console;break;}if(result==0xffffffff)result=sessions[i].SessionId;}
        WTSFreeMemory(sessions);
    }return result==0xffffffff?console:result;
}
static Handle interactiveSystemToken(DWORD session){
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(!snapshot)return {};
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);
    for(bool more=Process32FirstW(snapshot.value,&entry);more;more=Process32NextW(snapshot.value,&entry)){
        DWORD actual=0;if(_wcsicmp(entry.szExeFile,L"winlogon.exe") || !ProcessIdToSessionId(entry.th32ProcessID,&actual) || actual!=session)continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID));if(!process)continue;
        WCHAR path[32768]{},systemPath[MAX_PATH]{};DWORD length=DWORD(std::size(path));
        if(!QueryFullProcessImageNameW(process.value,0,path,&length)||!GetSystemDirectoryW(systemPath,MAX_PATH))continue;
        if(QString::fromWCharArray(path).compare(QString::fromWCharArray(systemPath)+"\\winlogon.exe",Qt::CaseInsensitive))continue;
        HANDLE raw=nullptr;if(!OpenProcessToken(process.value,TOKEN_QUERY|TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY,&raw))continue;
        Handle token(raw);DWORD tokenSession=0,n=0;
        if(tokenSid(raw)=="S-1-5-18" && GetTokenInformation(raw,TokenSessionId,&tokenSession,sizeof(tokenSession),&n) && tokenSession==session)return token;
    }return {};
}
std::unique_ptr<Child> Backend::spawn(bool system,DWORD session){
    auto child=std::make_unique<Child>();child->session=session;
    HANDLE raw=nullptr;Handle base;
    if(system){base=interactiveSystemToken(session);if(!base){m_state="等待交互会话的系统令牌";return {};}}
    else {if(!WTSQueryUserToken(session,&raw))return {};base.reset(raw);}
    HANDLE primary=nullptr;if(!DuplicateTokenEx(base.value,TOKEN_ALL_ACCESS,nullptr,SecurityImpersonation,TokenPrimary,&primary))return {};Handle token(primary);
    if(!system){TOKEN_LINKED_TOKEN linked{};DWORD n=0;TOKEN_ELEVATION elevation{};
        if(GetTokenInformation(token.value,TokenElevation,&elevation,sizeof(elevation),&n)&&elevation.TokenIsElevated && GetTokenInformation(token.value,TokenLinkedToken,&linked,sizeof(linked),&n))token.reset(linked.LinkedToken);
        else if(elevation.TokenIsElevated){BYTE admin[SECURITY_MAX_SID_SIZE]{};DWORD size=sizeof(admin);SID_AND_ATTRIBUTES deny{};HANDLE restricted=nullptr;
            if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin,&size))return {};deny.Sid=admin;
            if(!CreateRestrictedToken(token.value,DISABLE_MAX_PRIVILEGE,1,&deny,0,nullptr,0,nullptr,&restricted))return {};token.reset(restricted);
        }
        PSID medium=nullptr;
        if(!ConvertStringSidToSidW(L"S-1-16-8192",&medium))return {};
        TOKEN_MANDATORY_LABEL label{};label.Label.Sid=medium;label.Label.Attributes=SE_GROUP_INTEGRITY;
        const bool lowered=SetTokenInformation(token.value,TokenIntegrityLevel,&label,sizeof(label)+GetLengthSid(medium));LocalFree(medium);if(!lowered)return {};
    }else {
        // Remove unrelated privileges from the dedicated session token. Keep
        // trusted-session and traversal privileges in this fixed worker only;
        // no GUI, file broker or local IPC caller receives this token.
        DWORD bytes=0;GetTokenInformation(token.value,TokenPrivileges,nullptr,0,&bytes);
        if(!bytes)return {};QByteArray buffer(int(bytes),'\0');
        if(!GetTokenInformation(token.value,TokenPrivileges,buffer.data(),bytes,&bytes))return {};
        LUID tcb{},notify{};if(!LookupPrivilegeValueW(nullptr,SE_TCB_NAME,&tcb)||!LookupPrivilegeValueW(nullptr,SE_CHANGE_NOTIFY_NAME,&notify))return {};
        std::vector<LUID_AND_ATTRIBUTES> remove;auto privileges=reinterpret_cast<TOKEN_PRIVILEGES *>(buffer.data());
        for(DWORD i=0;i<privileges->PrivilegeCount;++i){const auto &entry=privileges->Privileges[i];
            const bool needed=(entry.Luid.LowPart==tcb.LowPart && entry.Luid.HighPart==tcb.HighPart)||(entry.Luid.LowPart==notify.LowPart && entry.Luid.HighPart==notify.HighPart);
            if(!needed)remove.push_back(entry);
        }
        QByteArray changes(int(sizeof(DWORD)+remove.size()*sizeof(LUID_AND_ATTRIBUTES)),'\0');
        auto policy=reinterpret_cast<TOKEN_PRIVILEGES *>(changes.data());policy->PrivilegeCount=DWORD(remove.size());
        for(size_t i=0;i<remove.size();++i){policy->Privileges[i]=remove[i];policy->Privileges[i].Attributes=SE_PRIVILEGE_REMOVED;}
        SetLastError(ERROR_SUCCESS);if(!AdjustTokenPrivileges(token.value,FALSE,policy,0,nullptr,nullptr)||GetLastError()!=ERROR_SUCCESS)return {};
    }
    const auto sid=tokenSid(token.value);child->sid=sid;const auto pipeName=(system?"LinkDeskHost-worker-":"LinkDeskHost-user-")+QUuid::createUuid().toString(QUuid::Id128);
    auto pipe=serverPipe(pipeName,system?QString():sid);if(!pipe)return {};
    child->job.reset(CreateJobObjectW(nullptr,nullptr));if(!child->job)return {};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION|JOB_OBJECT_LIMIT_ACTIVE_PROCESS;limits.BasicLimitInformation.ActiveProcessLimit=1;
    if(!SetInformationJobObject(child->job.value,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return {};
    const auto exe=QDir::toNativeSeparators(installDirectory()+"/LinkDeskWorker.exe");QString pathError;if(!trustedPath(installDirectory(),&pathError)||!trustedPath(exe,&pathError)){m_state=pathError;return {};}
    auto command=("\""+exe+"\" "+(system?"--capture":"--user-broker")+" "+pipeName+" "+QString::number(GetCurrentProcessId())+" "+(m_config.unattended?"secure":"ordinary")).toStdWString();
    STARTUPINFOW start{};start.cb=sizeof(start);auto desktop=(QStringLiteral("winsta0\\")+(system?m_workerDesktop:QStringLiteral("Default"))).toStdWString();start.lpDesktop=desktop.data();PROCESS_INFORMATION process{};LPVOID environment=nullptr;
    if(!CreateEnvironmentBlock(&environment,token.value,FALSE)){m_state=winError("创建会话环境");return {};}const auto working=QDir::toNativeSeparators(installDirectory()).toStdWString();
    const bool ok=CreateProcessAsUserW(token.value,reinterpret_cast<LPCWSTR>(exe.utf16()),command.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,environment,working.c_str(),&start,&process);
    if(environment)DestroyEnvironmentBlock(environment);if(!ok){m_state=winError("启动会话工作进程");return {};}
    child->process.reset(process.hProcess);Handle thread(process.hThread);child->pid=process.dwProcessId;child->token=std::move(token);
    if(!AssignProcessToJobObject(child->job.value,child->process.value)){TerminateProcess(child->process.value,1);return {};}
    const auto expected=child->pid;const auto expectedSid=sid;const auto processHandle=child->process.value;
    const auto generation=system?++m_workerGeneration:0;auto inbox=std::make_shared<WorkerInbox>();
    child->pipe=std::make_unique<PipeChannel>(std::move(pipe),true,[system,expected,expectedSid,processHandle,session](HANDLE p){ULONG pid=0;DWORD actualSession=0,code=0;
        if(!GetNamedPipeClientProcessId(p,&pid)||pid!=expected||!GetExitCodeProcess(processHandle,&code)||code!=STILL_ACTIVE||(!ProcessIdToSessionId(pid,&actualSession)||actualSession!=session))return false;
        HANDLE t=nullptr;Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!process||!OpenProcessToken(process.value,TOKEN_QUERY,&t))return false;Handle token(t);
        return tokenSid(t)==expectedSid && (system==(expectedSid=="S-1-5-18"));
    },[this,system,generation,inbox](Kind k,const QByteArray &b){
        if(!system){userMessage(k,b);return;}
        {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if(k==Kind::Image)for(auto i=inbox->queue.begin();i!=inbox->queue.end();)if(i->first==Kind::Image)i=inbox->queue.erase(i);else ++i;
            if(inbox->queue.size()>=128)inbox->overflow=true;else inbox->queue.emplace_back(k,b);
            if(inbox->scheduled)return;inbox->scheduled=true;
        }
        QMetaObject::invokeMethod(this,[this,generation,inbox]{
            std::deque<std::pair<Kind,QByteArray>> messages;bool overflow;
            {std::lock_guard<std::mutex> lock(inbox->mutex);messages.swap(inbox->queue);overflow=inbox->overflow;inbox->overflow=false;inbox->scheduled=false;}
            if(m_stopping || generation!=m_workerGeneration)return;
            if(overflow){invalidate(false);m_restartWorker=true;m_retry=0;return;}
            for(const auto &message:messages)workerMessage(message.first,message.second);
        },Qt::QueuedConnection);
    },[]{});
    ResumeThread(thread.value);return child;
}
void Backend::invalidate(bool expectNewEpoch){
    m_inputDesktop.clear();
    {std::lock_guard<std::mutex> lock(m_mutex);m_frame={};m_frame.pending=true;m_frame.waitReason="正在切换远端桌面";m_cursor={};m_boundaryPending=true;m_inputReady=false;if(expectNewEpoch)m_minEpoch=m_epoch+1;++m_boundaryVersion;}
    emit desktopBoundary();
}
void Backend::sessionEvent(DWORD event,DWORD session){if(session!=m_session)return;if(event==WTS_SESSION_LOGOFF){m_userBlocked=true;++m_authGeneration;if(!m_fileId.isEmpty())emit fileReply({{"type","file_error"},{"id",m_fileId},{"message","用户已注销，文件传输已停止"}});m_fileId.clear();stopChild(m_user);}else if(event==WTS_SESSION_LOGON||event==WTS_SESSION_UNLOCK)m_userBlocked=false;invalidate(true);releaseInput();if(m_worker && m_worker->pipe)m_worker->pipe->send(Kind::Command,json({{"type","rescan"}}));m_retry=0;}
void Backend::settings(){if(m_worker && m_worker->pipe)m_worker->pipe->send(Kind::Command,json({{"type","settings"},{"active",m_active},{"width",m_maximum.width()},{"height",m_maximum.height()},{"fps",m_fps}}));}
void Backend::supervise(){
    if(m_stopping)return;
    if(!m_control || !m_control->live())controlPipe();
    const auto session=selectSession();
    if(session!=m_session){++m_authGeneration;if(!m_fileId.isEmpty())emit fileReply({{"type","file_error"},{"id",m_fileId},{"message","交互用户会话已改变，请重新确认文件接收方"}});m_fileId.clear();invalidate(false);releaseInput();stopChild(m_user);stopChild(m_worker);m_session=session;m_workerDesktop="Default";m_restartWorker=false;m_userBlocked=false;m_epoch=0;m_minEpoch=0;m_retry=0;}
    if(m_session==0 || m_session==0xffffffff){m_state="尚无可用的交互控制台会话";return;}
    if((m_restartWorker || !m_worker || !m_worker->running()) && m_clock.elapsed()>=m_retry){
        m_restartWorker=false;
        invalidate(false);stopChild(m_worker);m_epoch=0;m_minEpoch=0;m_worker=spawn(true,m_session);m_retry=m_clock.elapsed()+2000;
        if(m_worker){settings();releaseInput();m_state="交互会话工作进程已启动";}
    }
    HANDLE token=nullptr;const bool userPresent=WTSQueryUserToken(m_session,&token);const auto loggedSid=token?tokenSid(token):QString();if(token)CloseHandle(token);
    if(loggedSid!=m_loggedSid){
        ++m_authGeneration;if(!m_fileId.isEmpty())emit fileReply({{"type","file_error"},{"id",m_fileId},{"message","登录用户已改变，请重新确认文件接收方"}});m_fileId.clear();stopChild(m_user);
        releaseInput();invalidate(true);if(m_worker && m_worker->pipe)m_worker->pipe->send(Kind::Command,json({{"type","rescan"}}));
        m_loggedSid=loggedSid;if(!loggedSid.isEmpty())m_userBlocked=false;
    }
    if(m_active && m_config.files && userPresent && !m_userBlocked){if(!m_user || !m_user->running()){if(!m_fileId.isEmpty())emit fileReply({{"type","file_retry"},{"id",m_fileId}});stopChild(m_user);m_user=spawn(false,m_session);}}
    else stopChild(m_user);
}
void Backend::activate(bool active){++m_authGeneration;m_fileId.clear();m_active=active;releaseInput();if(!active){stopChild(m_user);std::lock_guard<std::mutex> lock(m_mutex);m_frame={};m_frame.pending=true;m_frame.waitReason="等待认证连接";}supervise();settings();}
void Backend::setStream(int fps,const QSize &maximum){if(fps==m_fps && maximum==m_maximum)return;m_fps=qBound(1,fps,30);m_maximum=maximum;settings();}
CaptureFrame Backend::sample(const QSize &){std::lock_guard<std::mutex> lock(m_mutex);auto frame=m_frame;if(m_boundaryPending || !m_inputReady){frame.image={};frame.pending=true;if(frame.waitReason.isEmpty())frame.waitReason="等待新桌面的独立画面";}return frame;}
QRect Backend::geometry() const{std::lock_guard<std::mutex> lock(m_mutex);return m_geometry;}
CursorFrame Backend::cursor() const{std::lock_guard<std::mutex> lock(m_mutex);return m_cursor;}
void Backend::input(const QJsonObject &event){
    quint64 epoch;{std::lock_guard<std::mutex> lock(m_mutex);if(m_boundaryPending || !m_inputReady || m_frame.pending || !m_active)return;epoch=m_epoch;}
    const auto action=event.value("action").toString();
    if(action=="button" || action=="move"){
        const auto x=event.value("x"),y=event.value("y");if(!x.isDouble()||!y.isDouble()||!std::isfinite(x.toDouble())||!std::isfinite(y.toDouble())||x.toDouble()<0||x.toDouble()>1||y.toDouble()<0||y.toDouble()>1)return;
    }
    if(action=="key"){const int key=event.value("key").toInt();if(!event.value("key").isDouble()||!event.value("down").isBool()||!InputController::nativeKeySupported(key))return;if(event.value("down").toBool()){if(m_keys.size()>=32)return;m_keys.insert(key);}else m_keys.remove(key);}
    else if(action=="button"){const int b=event.value("button").toInt();if(b<1||b>3||!event.value("down").isBool())return;if(event.value("down").toBool())m_buttons.insert(b);else m_buttons.remove(b);}
    else if(action=="release"){releaseInput();return;}
    else if(action!="move" && action!="scroll")return;
    if(m_worker && m_worker->pipe && m_worker->pipe->send(Kind::Command,json({{"type","input"},{"epoch",double(epoch)},{"event",event}})))++m_inputForwarded;
}
void Backend::releaseInput(){
    m_inputReady=false;m_releaseKeys.unite(m_keys);m_releaseButtons.unite(m_buttons);m_keys.clear();m_buttons.clear();++m_releaseSerial;
    if(m_worker && m_worker->pipe){QJsonArray keys,buttons;for(auto k:m_releaseKeys)keys.append(k);for(auto b:m_releaseButtons)buttons.append(b);
        m_worker->pipe->send(Kind::Command,json({{"type","release_known"},{"serial",double(m_releaseSerial)},{"keys",keys},{"buttons",buttons}}));}
}
void Backend::workerMessage(Kind k,const QByteArray &b){
    if(k==Kind::Image){
        if(b.size()<32)return;const auto epoch=qFromBigEndian<quint64>(b.constData());qint32 f[6];for(int i=0;i<6;++i)f[i]=qint32(qFromBigEndian<quint32>(b.constData()+8+i*4));
        if(f[2]<1||f[3]<1||f[2]>32768||f[3]>32768||f[4]<1||f[5]<1||f[4]>4096||f[5]>4096||qint64(f[4])*f[5]>8000000||b.size()!=32+qint64(f[4])*f[5]*4)return;
        auto storage=new QByteArray(b.mid(32));QImage image(reinterpret_cast<const uchar *>(storage->constData()),f[4],f[5],f[4]*4,QImage::Format_RGB32,[](void *p){delete static_cast<QByteArray *>(p);},storage);
        std::lock_guard<std::mutex> lock(m_mutex);if(epoch!=m_epoch || epoch<m_minEpoch)return;m_frame.image=image;m_frame.pending=false;m_frame.waitReason.clear();m_geometry=QRect(f[0],f[1],f[2],f[3]);return;
    }
    if(k!=Kind::Event)return;const auto j=QJsonDocument::fromJson(b).object();const auto type=j.value("type").toString();
    if(type=="boundary"){
        const auto epoch=quint64(j.value("epoch").toDouble());quint64 version;
        {std::lock_guard<std::mutex> lock(m_mutex);if(epoch<m_minEpoch)return;m_epoch=epoch;m_frame={};m_frame.pending=true;m_cursor={};m_boundaryPending=true;m_inputReady=false;version=++m_boundaryVersion;}
        releaseInput();emit desktopBoundary();{std::lock_guard<std::mutex> lock(m_mutex);if(version==m_boundaryVersion)m_boundaryPending=false;}
    }else if(type=="desktop_rebind"){
        const auto name=j.value("desktop").toString();const auto epoch=quint64(j.value("epoch").toDouble());
        if(name!="Default" && (name!="Winlogon" || !m_config.unattended))return;
        if(epoch==m_epoch && name!=m_workerDesktop){m_workerDesktop=name;m_restartWorker=true;m_retry=0;}
    }else if(type=="release_applied"){
        const auto epoch=quint64(j.value("epoch").toDouble()),serial=quint64(j.value("serial").toDouble());const bool failed=j.value("error").toDouble()!=0;
        if(!failed && epoch==m_epoch && serial==m_releaseSerial){m_releaseKeys.clear();m_releaseButtons.clear();m_inputReady=true;}
    }else if(type=="input_result"){
        const auto epoch=quint64(j.value("epoch").toDouble());const auto error=DWORD(j.value("error").toDouble());
        if(epoch==m_epoch){++m_inputApplied;m_inputError=error;}
    }else if(type=="desktop_ready"){
        const auto name=j.value("desktop").toString().left(256);const auto epoch=quint64(j.value("epoch").toDouble());{std::lock_guard<std::mutex> lock(m_mutex);if(epoch==m_epoch)m_inputDesktop=name;}
    }else if(type=="wait"){
        std::lock_guard<std::mutex> lock(m_mutex);m_frame.image={};m_frame.pending=true;m_frame.waitReason=j.value("message").toString().left(512);m_cursor={};
    }else if(type=="cursor"){
        const auto data=QByteArray::fromBase64(j.value("image").toString().toLatin1());QImage image;if(!data.isEmpty()){QBuffer buffer;buffer.setData(data);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"PNG");const auto size=reader.size();if(size.width()<1||size.width()>128||size.height()<1||size.height()>128)return;image=reader.read();}
        std::lock_guard<std::mutex> lock(m_mutex);if(quint64(j.value("epoch").toDouble())!=m_epoch||m_boundaryPending)return;m_cursor={image,QPoint(j.value("hx").toInt(),j.value("hy").toInt()),QPointF(j.value("x").toDouble(),j.value("y").toDouble()),j.value("visible").toBool()};
    }
}
void Backend::userMessage(Kind k,const QByteArray &b){
    const auto generation=m_authGeneration.load();
    if(k==Kind::Event){const auto j=QJsonDocument::fromJson(b).object();const auto path=j.value("path").toString();if(j.value("type")=="user_ready"){QMetaObject::invokeMethod(this,[this,generation]{if(generation==m_authGeneration){m_userReady=true;if(!m_fileId.isEmpty())emit fileReply({{"type","file_retry"},{"id",m_fileId}});}},Qt::QueuedConnection);return;}if(j.value("type")=="file_complete" && !path.isEmpty() && path.size()<=32768)QMetaObject::invokeMethod(this,[this,path,generation]{if(generation==m_authGeneration && m_user && m_user->sid==m_config.operatorSid){m_completedPath=path;++m_completedSequence;}},Qt::QueuedConnection);return;}
    if(k==Kind::FileChunk){QMetaObject::invokeMethod(this,[this,b,generation]{if(m_active && generation==m_authGeneration)emit fileChunk(b);},Qt::QueuedConnection);return;}
    if(k==Kind::FileJson){const auto j=QJsonDocument::fromJson(b).object();if(allowedFile(j))QMetaObject::invokeMethod(this,[this,j,generation]{if(m_active && generation==m_authGeneration)emit fileReply(j);},Qt::QueuedConnection);}
}
void Backend::fileJson(const QJsonObject &j){if(!allowedFile(j))return;const auto type=j.value("type").toString();if(type=="file_offer")m_fileId=j.value("id").toString();if(m_config.files && m_active && !m_userReady){HANDLE token=nullptr;const bool logged=!m_userBlocked && WTSQueryUserToken(m_session,&token);if(token)CloseHandle(token);emit fileReply({{"type",logged?"file_retry":"file_error"},{"id",j.value("id")},{"message","文件访问需要已登录用户"}});return;}if(m_config.files && m_active && m_user && m_user->pipe && m_user->pipe->send(Kind::FileJson,json(j)))return;emit fileReply({{"type","file_error"},{"id",j.value("id")},{"message","文件传输需要已登录用户及单独授权；系统服务不访问用户文件"}});}
void Backend::fileData(const QByteArray &b){if(m_config.files && m_active && m_userReady && m_user && m_user->pipe)m_user->pipe->send(Kind::FileChunk,b);}
bool Backend::power(const QString &action,QString *error){if(!m_config.power || (action!="restart" && action!="shutdown")){if(error)*error="管理员尚未单独授权电源操作";return false;}if(!enablePrivilege(SE_SHUTDOWN_NAME)){if(error)*error=winError("Authorize power operation");return false;}if(!InitiateSystemShutdownExW(nullptr,nullptr,30,FALSE,action=="restart",SHTDN_REASON_MAJOR_APPLICATION|SHTDN_REASON_FLAG_PLANNED)){if(error)*error=winError("Power operation");return false;}return true;}
bool Backend::sasAvailable() const{DWORD value=0,n=sizeof(value);return m_config.sas && m_config.unattended && RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",L"SoftwareSASGeneration",RRF_RT_REG_DWORD,nullptr,&value,&n)==ERROR_SUCCESS && (value==1 || value==3);}
bool Backend::sas(QString *error){
    if(!sasAvailable() || !m_worker || !m_worker->token){if(error)*error="SAS 未授权或 Windows 策略未允许服务生成 SAS";return false;}
    HMODULE module=LoadLibraryExW(L"sas.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto call=module?reinterpret_cast<void(WINAPI *)(BOOL)>(GetProcAddress(module,"SendSAS")):nullptr;
    if(!call){if(module)FreeLibrary(module);if(error)*error="系统 SAS 接口不可用";return false;}
    // A service invokes SAS directly for the active console. Impersonation
    // is needed only to select a different explicitly selected session.
    const bool console=m_session==WTSGetActiveConsoleSessionId();
    HANDLE raw=nullptr;Handle user;DWORD session=0,n=0;
    if(!console && WTSQueryUserToken(m_session,&raw)){user.reset(raw);
        if(!GetTokenInformation(raw,TokenSessionId,&session,sizeof(session),&n) || session!=m_session){FreeLibrary(module);if(error)*error="SAS 会话校验失败";return false;}
    }
    const bool impersonated=console || ImpersonateLoggedOnUser(user?user.value:m_worker->token.value);
    DWORD failure=impersonated?ERROR_SUCCESS:GetLastError();
    if(impersonated){call(FALSE);if(!console && !RevertToSelf()){
        // Never continue the service event loop with an impersonated token.
        TerminateProcess(GetCurrentProcess(),ERROR_ACCESS_DENIED);
    }}
    FreeLibrary(module);if(!impersonated && error){SetLastError(failure);*error=winError("Select SAS session");}return impersonated;
}
void Backend::setHostStatus(bool online,const QString &id,const QString &fingerprint,bool sharing,bool connected){m_online=online;m_id=id;m_fingerprint=fingerprint;m_sharing=sharing;m_connected=connected;}
QJsonObject Backend::status() const{return {{"version",1},{"inputForwarded",double(m_inputForwarded)},{"inputApplied",double(m_inputApplied)},{"inputError",double(m_inputError)},{"server",m_config.server},{"port",int(m_config.port)},{"deviceId",m_id},{"online",m_online},{"fingerprint",m_fingerprint},{"sharing",m_sharing},{"connected",m_connected},{"unattended",m_config.unattended},{"session",double(m_session)},{"desktop",m_inputDesktop},{"workerPid",m_worker?double(m_worker->pid):0},{"userBrokerPid",m_user?double(m_user->pid):0},{"sas",sasAvailable()},{"state",m_migrationPending?QString("等待中转服务器确认机器身份迁移；将自动重试"):!m_hostError.isEmpty()?m_hostError:m_state},{"completedFile",m_completedPath},{"completedSequence",double(m_completedSequence)}};}
void Backend::controlPipe(){
    ++m_controlGeneration;m_control.reset();auto pipe=serverPipe("LinkDeskHost-control-v1",m_config.operatorSid);if(!pipe)return;const auto raw=pipe.value;const auto generation=m_controlGeneration;
    m_control=std::make_unique<PipeChannel>(std::move(pipe),true,std::function<bool(HANDLE)>(),[this,raw,generation](Kind k,const QByteArray &b){
        if(k!=Kind::Control || !authorizedCaller(raw,m_config.operatorSid))return;
        const auto j=QJsonDocument::fromJson(b).object();const auto command=j.value("command").toString();if(j.size()!=2 || j.value("version").toInt()!=1 || (command!="status" && command!="pause" && command!="resume" && command!="disconnect"))return;
        QMetaObject::invokeMethod(this,[this,command,generation]{if(generation!=m_controlGeneration)return;if(command!="status" && control)control(command);if(m_control)m_control->send(Kind::Control,json(status()));},Qt::QueuedConnection);
    },[]{});
}
}
