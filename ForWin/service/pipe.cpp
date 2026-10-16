#include "pipe.h"
#include <sddl.h>
#include <QJsonDocument>
#include <QtEndian>
#include <QElapsedTimer>
namespace HostService {
quint32 limit(Kind k){switch(k){case Kind::Image:return 32*1024*1024;case Kind::Event:return 65536;case Kind::FileChunk:return 32840;case Kind::Command:case Kind::FileJson:return 16384;case Kind::Control:return 4096;}return 0;}
Handle serverPipe(const QString &name,const QString &sid,QString *error){
    QString sddl="O:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)";
    if(!sid.isEmpty())sddl+="(A;;0x12019b;;;"+sid+")"; // Deliberately omit FILE_CREATE_PIPE_INSTANCE.
    PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&sd,nullptr))return {};
    SECURITY_ATTRIBUTES a{sizeof(a),sd,FALSE};const auto path=("\\\\.\\pipe\\"+name).toStdWString();
    HANDLE h=CreateNamedPipeW(path.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,256*1024,256*1024,0,&a);
    LocalFree(sd);if(h==INVALID_HANDLE_VALUE){if(error)*error=winError("Create private pipe");return {}; }return Handle(h);
}
Handle clientPipe(const QString &name,DWORD expectedServer,DWORD timeout){
    const auto path=("\\\\.\\pipe\\"+name).toStdWString();QElapsedTimer clock;clock.start();
    while(clock.elapsed()<timeout){
        Handle pipe(CreateFileW(path.c_str(),FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|READ_CONTROL|SYNCHRONIZE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
        if(pipe){ULONG pid=0;if(expectedServer && GetNamedPipeServerProcessId(pipe.value,&pid) && pid==expectedServer && servicePid()==expectedServer)return pipe;return {};}
        if(GetLastError()!=ERROR_PIPE_BUSY && GetLastError()!=ERROR_FILE_NOT_FOUND)return {};
        WaitNamedPipeW(path.c_str(),50);Sleep(10);
    }return {};
}
bool authorizedCaller(HANDLE pipe,const QString &sid){
    if(!ImpersonateNamedPipeClient(pipe))return false;
    HANDLE h=nullptr;bool ok=false;
    if(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&h)){
        Handle token(h);const auto user=tokenSid(h);BYTE admin[SECURITY_MAX_SID_SIZE]{};DWORD size=sizeof(admin);BOOL member=FALSE;
        if(CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admin,&size))CheckTokenMembership(h,admin,&member);
        ok=(!sid.isEmpty() && user==sid)||user=="S-1-5-18"||member;
    }
    if(!RevertToSelf())TerminateProcess(GetCurrentProcess(),ERROR_ACCESS_DENIED);return ok;
}
PipeChannel::PipeChannel(Handle pipe,bool server,std::function<bool(HANDLE)> verify,Receive receive,std::function<void()> closed)
 :m_pipe(std::move(pipe)),m_stop(CreateEventW(nullptr,TRUE,FALSE,nullptr)),m_server(server),m_verify(std::move(verify)),m_receive(std::move(receive)),m_closed(std::move(closed)){
    m_reader.reset(QThread::create([this]{readLoop();}));m_writer.reset(QThread::create([this]{writeLoop();}));m_reader->start();m_writer->start();
}
PipeChannel::~PipeChannel(){stop();if(m_reader)m_reader->wait();if(m_writer)m_writer->wait();if(m_server&&m_pipe)DisconnectNamedPipe(m_pipe.value);}
void PipeChannel::stop(){if(m_stopping.exchange(true))return;SetEvent(m_stop.value);CancelIoEx(m_pipe.value,nullptr);m_condition.notify_all();}
bool PipeChannel::send(Kind k,const QByteArray &b){if(b.isEmpty() || quint64(b.size())>limit(k) || m_stopping)return false;std::lock_guard<std::mutex> guard(m_mutex);
    if(k==Kind::Image)for(auto i=m_queue.begin();i!=m_queue.end();)if(i->first==k){m_bytes-=i->second.size();i=m_queue.erase(i);}else ++i;
    if(m_bytes+b.size()>40*1024*1024)return false;m_queue.emplace_back(k,b);m_bytes+=b.size();m_condition.notify_all();return true;
}
bool PipeChannel::transfer(bool writing,char *buffer,DWORD size){
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
    DWORD done=0;while(done<size && !m_stopping){OVERLAPPED ov{};ov.hEvent=event.value;ResetEvent(event.value);DWORD n=0;
        const DWORD amount=qMin<DWORD>(size-done,256*1024);
        BOOL ok=writing ? WriteFile(m_pipe.value,buffer+done,amount,&n,&ov) : ReadFile(m_pipe.value,buffer+done,amount,&n,&ov);
        if(!ok && GetLastError()==ERROR_IO_PENDING){HANDLE waits[]{m_stop.value,event.value};const auto result=WaitForMultipleObjects(2,waits,FALSE,writing?5000:INFINITE);
            if(result!=WAIT_OBJECT_0+1){CancelIoEx(m_pipe.value,&ov);GetOverlappedResult(m_pipe.value,&ov,&n,TRUE);return false;}
            ok=GetOverlappedResult(m_pipe.value,&ov,&n,FALSE);
        }
        if(!ok || !n)return false;done+=n;
    }return done==size;
}
void PipeChannel::readLoop(){
    bool connected=true;
    if(m_server){Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED ov{};ov.hEvent=event.value;BOOL ok=ConnectNamedPipe(m_pipe.value,&ov);auto e=ok?ERROR_SUCCESS:GetLastError();
        if(!ok && e==ERROR_IO_PENDING){HANDLE waits[]{m_stop.value,event.value};auto result=WaitForMultipleObjects(2,waits,FALSE,10000);DWORD n=0;if(result!=WAIT_OBJECT_0+1){CancelIoEx(m_pipe.value,&ov);GetOverlappedResult(m_pipe.value,&ov,&n,TRUE);connected=false;}else connected=GetOverlappedResult(m_pipe.value,&ov,&n,FALSE);}
        else connected=ok || e==ERROR_PIPE_CONNECTED;
    }
    if(connected && m_verify)connected=m_verify(m_pipe.value);
    if(connected){std::lock_guard<std::mutex> guard(m_mutex);m_connected=true;m_condition.notify_all();}
    while(connected && !m_stopping){char header[5]{};if(!transfer(false,header,sizeof(header)))break;const auto k=Kind(quint8(header[0]));const auto n=qFromBigEndian<quint32>(header+1);if(!n || n>limit(k))break;QByteArray b(int(n),Qt::Uninitialized);if(!transfer(false,b.data(),n))break;
        if(m_receive)m_receive(k,b);
    }
    stop();if(m_closed)m_closed();
}
void PipeChannel::writeLoop(){while(!m_stopping){std::pair<Kind,QByteArray> item;
    {std::unique_lock<std::mutex> lock(m_mutex);m_condition.wait(lock,[this]{return m_stopping || (m_connected && !m_queue.empty());});if(m_stopping)break;item=std::move(m_queue.front());m_queue.pop_front();m_bytes-=item.second.size();}
    QByteArray packet(5,'\0');packet[0]=char(item.first);qToBigEndian<quint32>(quint32(item.second.size()),packet.data()+1);packet+=item.second;if(!transfer(true,packet.data(),DWORD(packet.size()))){stop();break;}
}}
bool controlCall(const QString &command,QJsonObject *reply,QString *error){
    if(command!="status" && command!="pause" && command!="resume" && command!="disconnect"){if(error)*error="Unsupported service command";return false;}
    const DWORD pid=servicePid();if(!pid){if(error)*error="Windows 服务未运行";return false;}
    auto pipe=clientPipe("LinkDeskHost-control-v1",pid,1000);if(!pipe){if(error)*error="Windows 服务控制通道不可访问";return false;}
    std::mutex mutex;std::condition_variable ready;bool got=false,valid=false;QJsonObject response;
    PipeChannel channel(std::move(pipe),false,{},[&](Kind k,const QByteArray &b){std::lock_guard<std::mutex> lock(mutex);if(k==Kind::Control){response=QJsonDocument::fromJson(b).object();valid=response.value("version").toInt()==1;}got=true;ready.notify_all();},[&]{std::lock_guard<std::mutex> lock(mutex);got=true;ready.notify_all();});
    channel.send(Kind::Control,QJsonDocument(QJsonObject{{"version",1},{"command",command}}).toJson(QJsonDocument::Compact));
    {std::unique_lock<std::mutex> lock(mutex);ready.wait_for(lock,std::chrono::milliseconds(2000),[&]{return got;});if(reply)*reply=response;}
    channel.stop();if(!valid && error)*error="Windows 服务响应超时或身份不匹配";return valid;
}
QString queryStatus(QString *error){QJsonObject reply;if(!controlCall("status",&reply,error))return {};return QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));}
}
