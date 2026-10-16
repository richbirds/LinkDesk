#pragma once
#include "hostbackend.h"
#include "native.h"
#include "pipe.h"
#include <QTimer>
#include <QElapsedTimer>
#include <memory>
namespace HostService {
struct Child {
    Handle process,job,token;QString sid;DWORD pid=0,session=0xffffffff;
    std::unique_ptr<PipeChannel> pipe;
    bool running() const {DWORD code=0;return process && GetExitCodeProcess(process.value,&code) && code==STILL_ACTIVE && pipe && pipe->live();}
};
class Backend final : public HostBackend {
    Q_OBJECT
public:
    explicit Backend(const Config &,QObject *parent=nullptr);
    ~Backend() override;
    CaptureFrame sample(const QSize &) override;
    QRect geometry() const override;
    CursorFrame cursor() const override;
    void setStream(int fps,const QSize &) override;
    void activate(bool) override;
    void input(const QJsonObject &) override;
    void releaseInput() override;
    void fileJson(const QJsonObject &) override;
    void fileData(const QByteArray &) override;
    bool filesAllowed() const override{return m_config.files;}
    bool power(const QString &,QString *) override;
    bool sasAvailable() const override;
    bool sas(QString *) override;
    void sessionEvent(DWORD event,DWORD session);
    void shutdown();
    QJsonObject status() const;
    void setHostError(const QString &error){m_hostError=error;}
    void setMigrationPending(bool pending){m_migrationPending=pending;}
    void setHostStatus(bool online,const QString &id,const QString &fingerprint,bool sharing,bool connected=false);
    std::function<void(const QString &)> control;
private:
    Config m_config;
    std::unique_ptr<Child> m_worker,m_user;
    std::unique_ptr<PipeChannel> m_control;
    QTimer m_watch;QElapsedTimer m_clock;qint64 m_retry=0;
    mutable std::mutex m_mutex;
    CaptureFrame m_frame;CursorFrame m_cursor;QRect m_geometry{0,0,1280,720};
    quint64 m_workerGeneration=0,m_controlGeneration=0,m_inputForwarded=0,m_inputApplied=0;DWORD m_inputError=0;
    quint64 m_epoch=0,m_minEpoch=0,m_boundaryVersion=0;
    bool m_active=false,m_stopping=false,m_boundaryPending=true;std::atomic<bool> m_inputReady{false};
    QSize m_maximum{480,270};int m_fps=1;
    DWORD m_session=0xffffffff;
    std::atomic<quint64> m_authGeneration{0};
    QSet<int> m_keys,m_buttons,m_releaseKeys,m_releaseButtons;quint64 m_releaseSerial=0;
    QString m_loggedSid,m_fileId;bool m_userReady=false;
    QString m_completedPath;quint64 m_completedSequence=0;
    QString m_hostError,m_inputDesktop,m_workerDesktop="Default";bool m_restartWorker=false;
    QString m_state="等待控制台会话",m_id,m_fingerprint;
    bool m_migrationPending=false;
    bool m_online=false,m_sharing=false,m_connected=false,m_userBlocked=false;
    void supervise();DWORD selectSession() const;
    std::unique_ptr<Child> spawn(bool system,DWORD session);
    void stopChild(std::unique_ptr<Child> &);
    void workerMessage(Kind,const QByteArray &);
    void userMessage(Kind,const QByteArray &);
    void invalidate(bool expectNewEpoch);
    void settings();void controlPipe();
};
}
