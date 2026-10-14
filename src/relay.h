#pragma once
#include <QObject>
#include <QPointer>
#include <QJsonObject>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QSslSocket>
#include <functional>
class Wire;
struct RelayConfig {
    QString host, fingerprint, enrollmentKey;
    quint16 port = 443;
    bool valid(QString *error = nullptr) const;
    static QString normalizePin(QString value);
};

// Bridges opaque inner TLS bytes through a verified outer TLS relay connection.
class RelayTunnel : public QObject {
    Q_OBJECT
public:
    RelayTunnel(const RelayConfig &config, const QJsonObject &request, QObject *parent=nullptr);
    void startViewer();
    void startHost(quint16 hostPort);
    void close();
signals:
    void viewerReady(quint16 localPort, const QString &fingerprint);
    void error(const QString &reason);
    void closed();
private:
    RelayConfig m_config;
    QJsonObject m_request;
    QSslSocket m_remote;
    QTcpServer m_listener;
    QPointer<QTcpSocket> m_local;
    QTimer m_deadline;
    QByteArray m_handshake;
    bool m_ready=false, m_closed=false, m_viewer=false, m_draining=false;
    quint16 m_hostPort=0;
    void start();
    void receive();
    void pumpToRemote();
    void pumpToLocal();
    void attach(QTcpSocket *socket);
    void fail(const QString &reason);
    void finish(bool remoteEnded);
};

class RelayAgent : public QObject {
    Q_OBJECT
public:
    explicit RelayAgent(QObject *parent=nullptr);
    ~RelayAgent();
    void start(const RelayConfig &config, const QString &identityFile = {});
    void stop();
    void setAvailable(quint16 localHostPort, const QString &fingerprint = {});
    void endHosting();
    void connectDevice(const QString &target);
    void cancelViewer();
    void connectViaRelay(const QString &target);
    bool online() const { return m_online; }
    QString deviceId() const { return m_id; }
    const RelayConfig &config() const { return m_config; }
    RelayTunnel *createViewerTunnel(const QString &target, QObject *owner);
    void resolveDevice(const QString &target);
signals:
    void onlineChanged(bool online, const QString &deviceId);
    void status(const QString &message);
    void error(const QString &message);
    void viewerReady(quint16 localPort, const QString &target, const QString &fingerprint);
    void viewerClosed();
    void incomingViewer(const QString &deviceId);
    void directReady(const QString &host,quint16 port,const QString &target,const QString &fingerprint);
    void peerResolved(const QJsonObject &message);
private:
    RelayConfig m_config;
    QString m_identityFile, m_id, m_token, m_peerFingerprint;
    QString m_instance;
    QPointer<QSslSocket> m_socket;
    QPointer<Wire> m_wire;
    QTimer m_reconnect, m_heartbeat, m_deadline;
    QHash<QString,QPointer<RelayTunnel>> m_incoming;
    QPointer<RelayTunnel> m_outgoing;
    quint16 m_localPort=0;
    bool m_enabled=false, m_online=false;
    int m_retry=0;
    bool m_lanSupported=false, m_directActive=false;
    int m_lookupGeneration=0;
    QString m_pendingTarget;
    QPointer<QSslSocket> m_probe;
    void lookupPeer(const QString &target);
    void probePeer(const QString &host,quint16 port,const QString &target,const QString &fingerprint,std::function<void()> fallback);
    bool loadIdentity(QString *error);
    bool saveIdentity(QString *error);
    void dial();
    void lost();
    QJsonObject credentials(const QString &type) const;
};
