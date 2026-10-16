#pragma once
#include "native.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <QThread>
#include <memory>
namespace HostService {
enum class Kind : unsigned char {Command=1,Image=2,Event=3,FileJson=4,FileChunk=5,Control=6};
quint32 limit(Kind kind);
Handle serverPipe(const QString &name,const QString &sid={},QString *error=nullptr);
Handle clientPipe(const QString &name,DWORD expectedServer,DWORD timeout=3000);
bool authorizedCaller(HANDLE pipe,const QString &sid);
class PipeChannel {
public:
    using Receive=std::function<void(Kind,const QByteArray &)>;
    PipeChannel(Handle pipe,bool server,std::function<bool(HANDLE)> verify,Receive receive,std::function<void()> closed);
    ~PipeChannel();
    bool send(Kind,const QByteArray &);
    void stop();bool live() const{return !m_stopping;}
private:
    Handle m_pipe,m_stop;bool m_server;
    std::function<bool(HANDLE)> m_verify;Receive m_receive;std::function<void()> m_closed;
    std::atomic<bool> m_stopping{false};std::unique_ptr<QThread> m_reader,m_writer;
    std::mutex m_mutex;std::condition_variable m_condition;
    std::deque<std::pair<Kind,QByteArray>> m_queue;qsizetype m_bytes=0;bool m_connected=false;
    void readLoop();void writeLoop();bool transfer(bool write,char *buffer,DWORD size);
};
bool controlCall(const QString &command,QJsonObject *reply,QString *error=nullptr);
}
