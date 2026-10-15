#pragma once
#include "input.h"
#include "filetransfer.h"
#include "desktopcodec.h"
#include "videocodec.h"
#include "streamscheduler.h"
#include <QTcpServer>
#include <QSslCertificate>
#include <QSslKey>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>
#include <QQueue>
#include <QThread>
class Wire;
class QSslSocket;
struct StreamSettings {
    int fps=10, quality=88, width=1920, height=1080;
    bool lossless=false;
    QString codec="auto";
    QJsonObject json() const;
    static StreamSettings fromJson(const QJsonObject &json);
};
class Listener : public QTcpServer {
    Q_OBJECT
public: using QTcpServer::QTcpServer;
signals: void accepted(qintptr descriptor);
protected: void incomingConnection(qintptr descriptor) override { emit accepted(descriptor); }
};
class RemoteHost : public QObject {
    Q_OBJECT
public:
    explicit RemoteHost(QObject *parent = nullptr);
    ~RemoteHost();
    bool start(quint16 port, QString *error, bool testMode = false, const QHostAddress &bind = QHostAddress::Any);
    void stop();
    void disconnectPeer();
    bool setControl(bool enabled, QString *error = nullptr);
    bool isListening() const { return m_listener.isListening(); }
    quint16 port() const { return m_listener.serverPort(); }
    QString code() const { return m_code; }
    QString fingerprint() const;
    void rotateCode();
    bool setCode(const QString &code,QString *error=nullptr);
    void setSyntheticFrame(const QImage &image) { if(m_testMode) m_syntheticFrame=image; }
    QJsonObject streamingMetrics() const { auto metrics=m_scheduler.metrics(m_frameClock.isValid() ? m_frameClock.elapsed() : 0);metrics["transportBytes"]=double(m_transportBytes);metrics["videoFrames"]=double(m_videoFrames);metrics["videoFallbacks"]=double(m_videoFallbacks);return metrics; }
signals:
    void status(const QString &message);
    void peerChanged(bool connected, const QString &name);
    void frameSent(qint64 bytes);
    void fileReceived(const QString &path);
    void fileProgress(const QString &name,qint64 done,qint64 total,const QString &state);
    void streamMetrics(const QJsonObject &metrics);
private:
    Listener m_listener;
    QSslCertificate m_cert;
    QSslKey m_key;
    QPointer<QSslSocket> m_peer;
    QPointer<Wire> m_wire;
    QSet<QSslSocket *> m_sockets;
    struct Attempts { int count = 0; qint64 since = 0; };
    QHash<QString, Attempts> m_attempts;
    QString m_code;
    QTimer m_capture, m_heartbeat;
    InputController m_input;
    QRect m_geometry;
    bool m_control = false, m_testMode = false;
    QThread m_encoderThread;
    QObject *m_encoder=nullptr;
    bool m_encoding=false;
    quint64 m_captureGeneration=0;
    qint64 m_lastCapture=-10000,m_lastFrameSent=-10000;
    QElapsedTimer m_frameClock;
    QQueue<QPair<qint64,int>> m_pendingFrames;
    StreamScheduler m_scheduler;
    QImage m_syntheticFrame;
    quint64 m_frameId=0;
    qint64 m_transportBytes=0,m_lastMetrics=0;
    qint64 m_videoFrames=0,m_videoFallbacks=0;
    QByteArray m_cursorShape;
    QJsonObject m_lastCursor;
    StreamSettings m_stream;
    bool loadIdentity(QString *error);
    void accept(qintptr descriptor);
    void capture();
};
class RemoteClient : public QObject {
    Q_OBJECT
public:
    explicit RemoteClient(QObject *parent = nullptr);
    void connectTo(const QString &host, quint16 port, const QString &code,
                   const QString &identity = {}, const QString &expectedFingerprint = {});
    void disconnectPeer();
    void sendInput(const QJsonObject &event);
    void sendFiles(const QStringList &paths);
    void cancelFiles() { m_files.cancel(); emit fileProgress({},0,0,"已取消发送"); }
    void requestPower(const QString &action);
    void trustCertificate(bool accepted);
    void setStreamSettings(const StreamSettings &settings);
    StreamSettings streamSettings() const { return m_stream; }
    bool active() const { return m_active; }
signals:
    void fileProgress(const QString &name,qint64 done,qint64 total,const QString &state);
    void powerResult(const QString &message);
    void certificateCheck(const QString &endpoint, const QString &sha256);
    void connected(const QString &name, bool control);
    void controlChanged(bool enabled);
    void disconnected();
    void error(const QString &message);
    void frame(const QByteArray &jpeg);
    void imageFrame(const QImage &image,qint64 wireBytes,bool changed);
    void streamSettingsApplied(const QJsonObject &settings);
    void cursorFrame(const QImage &image,const QPoint &hotspot,const QPointF &position,bool visible);
    void streamMetrics(const QJsonObject &metrics);
private:
    FileTransfer m_files{this};
    bool m_filesSupported=false;
    QPointer<QSslSocket> m_socket;
    QPointer<Wire> m_wire;
    QTimer m_timeout, m_heartbeat;
    QString m_code;
    bool m_active = false, m_trusting = false;
    DesktopDecoder m_desktopDecoder;
    bool m_streamSupported=false;
    StreamSettings m_stream;
    std::unique_ptr<VideoDecoder> m_videoDecoder;
    quint32 m_videoSequence=0;
    quint64 m_ackId=0;
    QImage m_cursorImage;
    QPoint m_cursorHotspot;
};
