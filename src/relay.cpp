#include "relay.h"
#include "protocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QDateTime>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QJsonArray>
#include <QSettings>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QtEndian>
#include <QUuid>

QString RelayConfig::normalizePin(QString value) { return value.trimmed().remove(':').remove(' ').toLower(); }
bool RelayConfig::valid(QString *error) const {
    QString reason;
    if (host.trimmed().isEmpty() || host.size()>253 || host.contains(QRegularExpression("[\\s/]"))) reason="请输入服务器 IP 或域名，不包含 https:// 前缀。";
    else if (!port) reason="服务器端口无效。";
    else if (!fingerprint.isEmpty() && !QRegularExpression("^[0-9a-f]{64}$").match(normalizePin(fingerprint)).hasMatch()) reason="服务器指纹必须是 64 位 SHA-256 十六进制，可包含冒号。";
    else if (enrollmentKey.size()<32 || enrollmentKey.size()>256) reason="服务器接入密钥长度应为 32–256 个字符。";
    if (error) *error=reason;
    return reason.isEmpty();
}

static QJsonArray localAddresses() {
    QJsonArray addresses;
    for(const auto &entry:QNetworkInterface::allAddresses()) {
        const auto value=entry.toIPv4Address();
        if (value && !entry.isLoopback() && (entry.isInSubnet(QHostAddress("10.0.0.0"),8) || entry.isInSubnet(QHostAddress("172.16.0.0"),12) || entry.isInSubnet(QHostAddress("192.168.0.0"),16))) addresses.append(entry.toString());
        if (addresses.size()>=8) break;
    }
    return addresses;
}
static bool sameSubnet(const QString &host) {
    const QHostAddress peer(host);
    for(const auto &interface:QNetworkInterface::allInterfaces()) for(const auto &entry:interface.addressEntries())
        if (entry.ip().protocol()==QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback() && entry.prefixLength()>=8 && peer.isInSubnet(entry.ip(),entry.prefixLength())) return true;
    return false;
}
static void configureSocket(QSslSocket *socket,const RelayConfig &config,QObject *owner,
                            std::function<void(const QString &)> failure,std::function<void()> verified) {
    auto ssl=socket->sslConfiguration(); ssl.setProtocol(QSsl::TlsV1_2OrLater); ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket->setSslConfiguration(ssl); socket->setSocketOption(QAbstractSocket::LowDelayOption,1); socket->setSocketOption(QAbstractSocket::KeepAliveOption,1); socket->setReadBufferSize(2*1024*1024);
    const auto pin=RelayConfig::normalizePin(config.fingerprint);
    QObject::connect(socket,&QSslSocket::sslErrors,owner,[socket,pin,failure](const QList<QSslError> &errors) {
        if (pin.isEmpty()) { failure("服务器 TLS 证书无法验证；自签名服务器需要填写部署时生成的指纹。"); return; }
        for (const auto &e : errors) {
            if (e.error()!=QSslError::SelfSignedCertificate && e.error()!=QSslError::CertificateUntrusted &&
                e.error()!=QSslError::HostNameMismatch && e.error()!=QSslError::UnableToGetLocalIssuerCertificate &&
                e.error()!=QSslError::UnableToVerifyFirstCertificate) {
                failure("服务器证书校验失败："+e.errorString()); return;
            }
        }
        socket->ignoreSslErrors(errors);
    });
    QObject::connect(socket,&QSslSocket::encrypted,owner,[socket,pin,failure,verified] {
        auto cert=socket->peerCertificate();
        if (cert.isNull() || (!pin.isEmpty() && QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex())!=pin)) {
            failure("服务器证书指纹不匹配，未发送设备凭据。"); return;
        }
        verified();
    });
}

RelayTunnel::RelayTunnel(const RelayConfig &config,const QJsonObject &request,QObject *parent)
    : QObject(parent),m_config(config),m_request(request) {
    m_deadline.setSingleShot(true);
    connect(&m_deadline,&QTimer::timeout,this,[this] { if (m_draining) close(); else fail("中继连接超时，请检查对方是否在线并开启共享。"); });
    connect(&m_remote,&QSslSocket::readyRead,this,&RelayTunnel::receive);
    connect(&m_remote,&QSslSocket::encryptedBytesWritten,this,[this] { pumpToRemote(); });
    connect(&m_remote,&QSslSocket::disconnected,this,[this] { finish(true); });
    connect(&m_remote,&QSslSocket::errorOccurred,this,[this](QAbstractSocket::SocketError e) {
        if (e != QAbstractSocket::RemoteHostClosedError) fail(m_remote.errorString());
    });
    connect(&m_listener,&QTcpServer::newConnection,this,[this] {
        auto socket=m_listener.nextPendingConnection(); m_listener.close(); attach(socket);
    });
    configureSocket(&m_remote,m_config,this,[this](const QString &s) { fail(s); },[this] {
        auto bytes=Wire::packet(Wire::Json,QJsonDocument(m_request).toJson(QJsonDocument::Compact));
        m_remote.write(bytes); m_request={};
    });
}
void RelayTunnel::startViewer() {
    m_viewer=true;
    if (!m_listener.listen(QHostAddress::LocalHost,0)) { fail(m_listener.errorString()); return; }
    start();
}
void RelayTunnel::startHost(quint16 hostPort) { m_hostPort=hostPort; start(); }
void RelayTunnel::start() { m_deadline.start(180000); m_remote.connectToHostEncrypted(m_config.host,m_config.port); }
void RelayTunnel::fail(const QString &reason) { if (!m_closed) { emit error(reason); close(); } }
void RelayTunnel::close() {
    if (m_closed) return; m_closed=true; m_deadline.stop(); m_listener.close();
    m_remote.abort(); if (m_local) m_local->abort(); m_request={}; emit closed(); deleteLater();
}
void RelayTunnel::finish(bool remoteEnded) {
    if (m_closed) return;
    if (m_draining || !m_ready || !m_local) { close(); return; }
    // Preserve final TLS records (including password rejection) before closing the other leg.
    if (remoteEnded) pumpToLocal(); else pumpToRemote();
    m_draining=true; m_deadline.start(5000);
    if (remoteEnded) m_local->disconnectFromHost(); else m_remote.disconnectFromHost();
}
void RelayTunnel::attach(QTcpSocket *socket) {
    if (!socket || m_closed) { if (socket) socket->deleteLater(); return; }
    m_local=socket; socket->setSocketOption(QAbstractSocket::LowDelayOption,1); socket->setParent(this); socket->setReadBufferSize(2*1024*1024);
    connect(socket,&QTcpSocket::readyRead,this,&RelayTunnel::pumpToRemote);
    connect(socket,&QTcpSocket::bytesWritten,this,[this] { pumpToLocal(); });
    connect(socket,&QTcpSocket::connected,this,[this] { pumpToLocal(); pumpToRemote(); });
    connect(socket,&QTcpSocket::disconnected,this,[this] { finish(false); });
    connect(socket,&QTcpSocket::errorOccurred,this,[this](QAbstractSocket::SocketError e) {
        if (e!=QAbstractSocket::RemoteHostClosedError) close();
    });
    if (socket->state()==QAbstractSocket::ConnectedState) { m_deadline.stop(); pumpToLocal(); pumpToRemote(); }
}
void RelayTunnel::receive() {
    if (m_closed) return;
    if (m_ready) { pumpToLocal(); return; }
    // Only the initial relay response is framed JSON. All following bytes are opaque TLS.
    while (!m_ready && m_remote.bytesAvailable()) {
        if (m_handshake.size()<5) m_handshake += m_remote.read(5-m_handshake.size());
        if (m_handshake.size()<5) return;
        auto length=qFromBigEndian<quint32>(m_handshake.constData()+1);
        if (quint8(m_handshake[0])!=Wire::Json || !length || length>Wire::MaxJson) { fail("中继响应格式无效。"); return; }
        m_handshake += m_remote.read(qsizetype(length)+5-m_handshake.size());
        if (m_handshake.size()<qsizetype(length)+5) return;
        QJsonParseError parseError;
        auto doc=QJsonDocument::fromJson(m_handshake.mid(5),&parseError); m_handshake.clear();
        if (parseError.error!=QJsonParseError::NoError || !doc.isObject()) { fail("中继响应无效。"); return; }
        auto j=doc.object();
        if (j.value("type")=="error") { fail(j.value("message").toString()); return; }
        if (j.value("type")!="ready") { fail("中继协议不兼容。"); return; }
        m_ready=true;
        if (m_viewer) {
            const auto fingerprint=j.value("fingerprint").toString();
            if (!QRegularExpression("^[0-9a-f]{64}$").match(fingerprint).hasMatch()) { fail("远程设备证书注册信息无效。"); return; }
            m_deadline.start(10000); emit viewerReady(m_listener.serverPort(),fingerprint);
        } else {
            auto local=new QTcpSocket(this); attach(local);
            local->connectToHost(QHostAddress::LocalHost,m_hostPort); m_deadline.start(10000);
        }
    }
    pumpToLocal();
}
void RelayTunnel::pumpToRemote() {
    if (m_closed || !m_ready || !m_local || !m_remote.isEncrypted()) return;
    while (m_local->bytesAvailable() && m_remote.bytesToWrite()+m_remote.encryptedBytesToWrite()<2*1024*1024) {
        auto data=m_local->read(128*1024);
        if (m_remote.write(data)!=data.size()) { close(); return; }
    }
}
void RelayTunnel::pumpToLocal() {
    if (m_closed || !m_ready || !m_local || m_local->state()!=QAbstractSocket::ConnectedState) return;
    m_deadline.stop();
    while (m_remote.bytesAvailable() && m_local->bytesToWrite()<2*1024*1024) {
        auto data=m_remote.read(128*1024);
        if (m_local->write(data)!=data.size()) { close(); return; }
    }
}

RelayAgent::RelayAgent(QObject *parent) : QObject(parent) {
    m_instance=QUuid::createUuid().toString(QUuid::Id128);
    auto diagnostic=[](const QString &message) {
        const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!QDir().mkpath(dir)) return;
        QFile file(dir+"/connection.log");
        if (file.size()>256*1024) { QFile::remove(dir+"/connection.previous.log"); file.rename(dir+"/connection.previous.log"); file.setFileName(dir+"/connection.log"); }
        if (file.open(QIODevice::WriteOnly|QIODevice::Append)) file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate)+" "+message+"\n").toUtf8());
    };
    connect(this,&RelayAgent::status,this,diagnostic);
    connect(this,&RelayAgent::error,this,diagnostic);
    m_reconnect.setSingleShot(true); connect(&m_reconnect,&QTimer::timeout,this,&RelayAgent::dial);
    m_deadline.setSingleShot(true); connect(&m_deadline,&QTimer::timeout,this,[this] { if (m_socket) m_socket->abort(); lost(); emit status("服务器响应超时，正在自动重连…"); });
    m_heartbeat.setInterval(3000); connect(&m_heartbeat,&QTimer::timeout,this,[this] { if (m_wire) m_wire->sendJson({{"type","ping"}}); });
}
RelayAgent::~RelayAgent() { stop(); }
bool RelayAgent::loadIdentity(QString *error) {
    QFile file(m_identityFile);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly)) { *error="无法读取本机设备凭据。"; return false; }
        auto j=QJsonDocument::fromJson(file.readAll()).object(); m_id=j.value("id").toString(); m_token=j.value("token").toString();
        if (!QRegularExpression("^[0-9a-f]{64}$").match(m_token).hasMatch() ||
            (!m_id.isEmpty() && !QRegularExpression("^[0-9]{9}$").match(m_id).hasMatch())) {
            *error="本机设备凭据文件损坏，请备份后移除该文件并重新连接。"; return false;
        }
        file.setPermissions(QFile::ReadOwner|QFile::WriteOwner); return true;
    }
    QByteArray token;
    for (int i=0;i<8;++i) { quint32 value=QRandomGenerator::system()->generate(); token.append(reinterpret_cast<const char *>(&value),sizeof(value)); }
    m_token=QString::fromLatin1(token.toHex()); m_id.clear(); return saveIdentity(error);
}
bool RelayAgent::saveIdentity(QString *error) {
    const auto dir=QFileInfo(m_identityFile).absolutePath();
    if (!QDir().mkpath(dir)) { *error="无法创建设备凭据目录。"; return false; }
    QFile::setPermissions(dir,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
    QSaveFile file(m_identityFile);
    if (!file.open(QIODevice::WriteOnly)) { *error="无法保存本机设备凭据。"; return false; }
    file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
    file.write(QJsonDocument(QJsonObject{{"id",m_id},{"token",m_token}}).toJson());
    if (!file.commit()) { *error="保存本机设备凭据失败。"; return false; } return true;
}
void RelayAgent::start(const RelayConfig &config,const QString &identityFile) {
    stop(); QString reason;
    if (!config.valid(&reason)) { emit error(reason); return; }
    m_config=config; m_id.clear(); m_token.clear();
    auto endpoint=config.host.toLower()+":"+QString::number(config.port);
    auto digest=QCryptographicHash::hash(endpoint.toUtf8(),QCryptographicHash::Sha256).toHex().left(16);
    m_identityFile=identityFile.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/devices/"+QString::fromLatin1(digest)+".json" : identityFile;
    if (!loadIdentity(&reason)) { emit error(reason); return; }
    m_enabled=true; m_retry=0; emit onlineChanged(false,m_id); dial();
}
void RelayAgent::stop() {
    m_enabled=false; m_reconnect.stop(); m_deadline.stop(); m_heartbeat.stop(); cancelViewer();
    const auto incoming=m_incoming; m_incoming.clear(); for (auto tunnel : incoming) if (tunnel) tunnel->close();
    if (m_socket) { auto socket=m_socket; m_socket.clear(); m_wire.clear(); socket->abort(); socket->deleteLater(); }
    m_online=false; emit onlineChanged(false,m_id);
}
QJsonObject RelayAgent::credentials(const QString &type) const {
    return {{"type",type},{"version",1},{"key",m_config.enrollmentKey},{"id",m_id},{"token",m_token}};
}
void RelayAgent::dial() {
    if (!m_enabled || m_socket) return;
    emit status("正在连接公网服务器…");
    auto socket=new QSslSocket(this); m_socket=socket;
    auto wire=new Wire(socket,socket); m_wire=wire;
    auto fail=[this,socket](const QString &s) { if (m_socket==socket) { socket->abort(); lost(); emit error(s); } };
    configureSocket(socket,m_config,this,fail,[this,socket,wire] {
        if (m_socket!=socket) return;
        auto j=credentials("register"); j["instance"]=m_instance; j["available"]=m_localPort!=0; j["fingerprint"]=m_peerFingerprint; j["lan"]=localAddresses(); j["lanPort"]=int(m_localPort); wire->sendJson(j);
    });
    connect(socket,&QSslSocket::errorOccurred,this,[fail,socket](QAbstractSocket::SocketError e) {
        if (e!=QAbstractSocket::RemoteHostClosedError) fail(socket->errorString());
    });
    connect(socket,&QSslSocket::disconnected,this,[this,socket] { if (m_socket==socket) lost(); socket->deleteLater(); });
    connect(wire,&Wire::invalidPacket,this,fail);
    connect(wire,&Wire::frameReceived,this,[fail](const QByteArray &) { fail("服务器返回了无效控制消息。"); });
    connect(wire,&Wire::jsonReceived,this,[this,socket,wire,fail](const QJsonObject &j) {
        if (m_socket!=socket) return;
        auto type=j.value("type").toString();
        if (type=="error") { fail(j.value("message").toString()); return; }
        if (type=="registered") {
            auto id=j.value("id").toString();
            if (!QRegularExpression("^[0-9]{9}$").match(id).hasMatch() || (!m_id.isEmpty() && m_id!=id)) { fail("服务器返回的设备 ID 无效。"); return; }
            m_id=id; QString reason; if (!saveIdentity(&reason)) { fail(reason); return; }
            m_online=true; m_lanSupported=j.value("lanDirect").toBool(); m_retry=0; m_heartbeat.start(); m_deadline.start(180000);
            emit onlineChanged(true,m_id); emit status("已连接公网服务器");
        } else if (m_online) {
            m_deadline.start(180000);
            if (type=="resolved") emit peerResolved(j);
            if (type=="resolved" && j.value("target").toString()==m_pendingTarget) {
                const auto target=m_pendingTarget,fp=j.value("fingerprint").toString();
                const int port=j.value("port").toInt();
                if (port>0 && port<=65535 && QRegularExpression("^[0-9a-f]{64}$").match(fp).hasMatch()) {
                    for(const auto &address:j.value("lan").toArray()) {
                        const auto host=address.toString();
                        if (sameSubnet(host)) { probePeer(host,quint16(port),target,fp,[this,target]{connectViaRelay(target);}); return; }
                    }
                }
                connectViaRelay(target);
            } else if (type=="incoming" && m_localPort) {
                const auto session=j.value("session").toString(),ticket=j.value("ticket").toString();
                if (session.size()!=32 || ticket.size()!=64 || !m_incoming.isEmpty()) return;
                auto request=credentials("join"); request["session"]=session; request["ticket"]=ticket;
                auto tunnel=new RelayTunnel(m_config,request,this); m_incoming.insert(session,tunnel);
                emit incomingViewer(j.value("viewer").toString());
                connect(tunnel,&RelayTunnel::error,this,&RelayAgent::error);
                connect(tunnel,&RelayTunnel::closed,this,[this,session,tunnel] { if (m_incoming.value(session)==tunnel) m_incoming.remove(session); });
                tunnel->startHost(m_localPort);
            } else if (type=="session_end") {
                auto tunnel=m_incoming.take(j.value("session").toString()); if (tunnel) tunnel->close();
            }
        }
    });
    m_deadline.start(45000); socket->connectToHostEncrypted(m_config.host,m_config.port);
}
void RelayAgent::lost() {
    if (!m_socket && !m_online && m_reconnect.isActive()) return;
    if (m_socket) { auto socket=m_socket; m_socket.clear(); m_wire.clear(); socket->abort(); socket->deleteLater(); }
    m_online=false; m_deadline.stop(); m_heartbeat.stop(); if (!m_directActive) cancelViewer();
    const auto incoming=m_incoming; m_incoming.clear(); for (auto tunnel : incoming) if (tunnel) tunnel->close();
    emit onlineChanged(false,m_id);
    if (m_enabled) { m_reconnect.start(qMin(60000,1000*(1<<qMin(m_retry++,6)))); emit status("服务器连接中断，正在自动重连…"); }
}
void RelayAgent::setAvailable(quint16 localHostPort,const QString &fingerprint) {
    m_localPort=localHostPort; m_peerFingerprint=RelayConfig::normalizePin(fingerprint);
    if (!localHostPort) endHosting();
    if (m_online && m_wire) m_wire->sendJson({{"type","available"},{"enabled",localHostPort!=0},{"fingerprint",m_peerFingerprint},{"lan",localAddresses()},{"lanPort",int(localHostPort)}});
}
void RelayAgent::endHosting() {
    const auto incoming=m_incoming; m_incoming.clear(); for (auto tunnel : incoming) if (tunnel) tunnel->close();
    if (m_online && m_wire) m_wire->sendJson({{"type","end"}});
}
void RelayAgent::connectDevice(const QString &target) {
    if (QSettings().value("network/forceRelay",false).toBool()) { connectViaRelay(target); return; }
    cancelViewer(); m_pendingTarget=target;
    const auto prefix="lan/"+m_config.host+":"+QString::number(m_config.port)+"/"+target+"/";
    QSettings settings; const auto host=settings.value(prefix+"host").toString(),fp=settings.value(prefix+"fingerprint").toString();
    const int port=settings.value(prefix+"port").toInt();
    if (sameSubnet(host) && port>0 && port<=65535 && QRegularExpression("^[0-9a-f]{64}$").match(fp).hasMatch()) {
        probePeer(host,quint16(port),target,fp,[this,target]{lookupPeer(target);}); return;
    }
    if (!m_online || !m_lanSupported) { connectViaRelay(target); return; }
    lookupPeer(target);
}
void RelayAgent::lookupPeer(const QString &target) {
    if (m_pendingTarget!=target) return;
    if (!m_online || !m_wire) { connectViaRelay(target); return; }
    const int generation=++m_lookupGeneration;
    emit status("正在查找局域网直连地址…"); m_wire->sendJson({{"type","resolve"},{"target",target}});
    QTimer::singleShot(45000,this,[this,target,generation]{if (m_lookupGeneration==generation && m_pendingTarget==target && !m_probe) connectViaRelay(target);});
}
RelayTunnel *RelayAgent::createViewerTunnel(const QString &target,QObject *owner) {
    auto request=credentials("connect"); request["target"]=target;
    return new RelayTunnel(m_config,request,owner);
}
void RelayAgent::resolveDevice(const QString &target) {
    if (m_online && m_wire) m_wire->sendJson({{"type","resolve"},{"target",target}});
}
void RelayAgent::probePeer(const QString &host,quint16 port,const QString &target,const QString &fp,std::function<void()> fallback) {
    if (m_pendingTarget!=target) return;
    auto socket=new QSslSocket(this); m_probe=socket;
    // A LAN peer must not be sent through a system HTTP/SOCKS proxy.
    socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
    auto config=m_config; config.host=host; config.port=port; config.fingerprint=fp;
    auto fail=[this,socket,target,fallback] { if (m_probe!=socket || m_pendingTarget!=target) return; m_probe.clear(); socket->abort(); socket->deleteLater(); fallback(); };
    configureSocket(socket,config,this,[this,fail](const QString &reason){emit status("局域网 TLS 校验失败："+reason);fail();},[this,socket,host,port,target,fp] {
        if (m_probe!=socket || m_pendingTarget!=target) return;
        m_probe.clear(); m_pendingTarget.clear(); m_directActive=true; socket->abort(); socket->deleteLater();
        const auto prefix="lan/"+m_config.host+":"+QString::number(m_config.port)+"/"+target+"/";
        QSettings settings; settings.setValue(prefix+"host",host); settings.setValue(prefix+"port",int(port)); settings.setValue(prefix+"fingerprint",fp);
        emit status("使用局域网直连"); emit directReady(host,port,target,fp);
    });
    connect(socket,&QSslSocket::errorOccurred,this,[this,socket,fail](QAbstractSocket::SocketError){emit status("局域网直连失败："+socket->errorString());fail();});
    QTimer::singleShot(2000,socket,[this,socket,fail]{if(m_probe==socket) { emit status("局域网探测超时，尝试中继。");fail(); }}); socket->connectToHostEncrypted(host,port);
}
void RelayAgent::connectViaRelay(const QString &target) {
    if (!m_online) { emit error("服务器尚未连接，请先配置服务器并等待在线。"); emit viewerClosed(); return; }
    cancelViewer(); auto request=credentials("connect"); request["target"]=target;
    auto tunnel=new RelayTunnel(m_config,request,this); m_outgoing=tunnel;
    connect(tunnel,&RelayTunnel::error,this,&RelayAgent::error);
    connect(tunnel,&RelayTunnel::viewerReady,this,[this,tunnel,target](quint16 port,const QString &fp) {
        if (m_outgoing==tunnel) emit viewerReady(port,target,fp);
    });
    connect(tunnel,&RelayTunnel::closed,this,[this,tunnel] {
        if (m_outgoing==tunnel) { m_outgoing.clear(); emit viewerClosed(); }
    });
    tunnel->startViewer();
}
void RelayAgent::cancelViewer() {
    m_pendingTarget.clear(); m_directActive=false; ++m_lookupGeneration; if (m_probe) { auto probe=m_probe; m_probe.clear(); probe->abort(); probe->deleteLater(); }
    if (m_outgoing) { auto tunnel=m_outgoing; m_outgoing.clear(); tunnel->close(); emit viewerClosed(); }
}
