#include "remote.h"
#include "protocol.h"
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostInfo>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QRandomGenerator>
#include <QScreen>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include "platform.h"
#include <QImageReader>

QJsonObject StreamSettings::json() const {
    return {{"fps",fps},{"quality",quality},{"width",width},{"height",height},{"lossless",lossless}};
}
StreamSettings StreamSettings::fromJson(const QJsonObject &j) {
    StreamSettings s;
    s.fps=qBound(1,j.value("fps").toInt(s.fps),30);
    s.quality=qBound(15,j.value("quality").toInt(s.quality),95);
    const int width=j.value("width").toInt(s.width);
    if (width==480) { s.width=480; s.height=270; }
    else if (width==1280) { s.width=1280; s.height=720; }
    else if (width==2560) { s.width=2560; s.height=1440; }
    s.lossless=j.value("lossless").toBool();
    return s;
}

static QString certHash(const QSslCertificate &cert) {
    return QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex(':')).toUpper();
}
static bool equalCode(const QString &a, const QString &b) {
    auto x = QCryptographicHash::hash(a.toUtf8(),QCryptographicHash::Sha256);
    auto y = QCryptographicHash::hash(b.toUtf8(),QCryptographicHash::Sha256);
    unsigned int diff = 0;
    for (int i=0; i<x.size(); ++i) diff |= quint8(x[i]) ^ quint8(y[i]);
    return diff == 0;
}
class CaptureEncoder : public QObject {
    Q_OBJECT
public:
    DesktopEncoder desktop; quint64 currentGeneration=~quint64(0);
    void commit(quint64 generation) { if(generation==currentGeneration) desktop.commit(); }
    void encode(quint64 generation,QImage image,const StreamSettings &settings,bool nativeCapture,bool incremental) {
        if(generation!=currentGeneration) { desktop.reset(); currentGeneration=generation; }
        if(nativeCapture) image=Platform::captureScreen(nullptr,QSize(settings.width,settings.height));
        if(image.isNull()) { emit ready(generation,{},"屏幕采集失败，请检查系统屏幕录制权限。"); return; }
        if(incremental) { auto bytes=desktop.encode(image,settings.quality,settings.lossless); if(bytes.isEmpty()) emit ready(generation,{},"增量画面编码失败。"); else emit ready(generation,bytes,{}); return; }
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if(!image.save(&buffer,settings.lossless ? "PNG" : "JPEG",settings.lossless ? -1 : settings.quality)) {
            emit ready(generation,{},"画面编码失败。"); return;
        }
        emit ready(generation,bytes,{});
    }
signals:
    void ready(quint64 generation,const QByteArray &bytes,const QString &error);
};
RemoteHost::RemoteHost(QObject *parent) : QObject(parent) {
    rotateCode();
    m_capture.setInterval(100);
    m_heartbeat.setInterval(3000);
    connect(&m_listener,&Listener::accepted,this,&RemoteHost::accept);
    connect(&m_capture,&QTimer::timeout,this,&RemoteHost::capture);
    connect(&m_heartbeat,&QTimer::timeout,this,[this] {
        if (m_wire) m_wire->sendJson({{"type","ping"}});
    });
    auto encoder=new CaptureEncoder; m_encoder=encoder; encoder->moveToThread(&m_encoderThread);
    connect(&m_encoderThread,&QThread::finished,encoder,&QObject::deleteLater);
    connect(encoder,&CaptureEncoder::ready,this,[this,encoder](quint64 generation,const QByteArray &bytes,const QString &error) {
        if(generation!=m_captureGeneration) return;
        m_encoding=false; if(!m_peer || !m_wire) return;
        if(!error.isEmpty()) { m_wire->sendJson({{"type","error"},{"message",error}}); disconnectPeer(); return; }
        if(bytes.size()>Wire::MaxFrame) { m_wire->sendJson({{"type","error"},{"message","无损画面过大，请降低分辨率或选择智能增量模式。"}}); disconnectPeer(); return; }
        const bool incremental=m_peer->property("desktopDelta").toBool();
        if(incremental && bytes.size()==19 && m_frameClock.elapsed()-m_lastFrameSent<1000) return;
        qint64 inFlight=0; for(const auto &pending:m_pendingFrames) inFlight+=pending.second;
        const qint64 limit=m_stream.lossless ? qint64(Wire::MaxFrame)*2 : 8*1024*1024;
        if(bytes.size()<=Wire::MaxFrame && inFlight+bytes.size()<=limit && (incremental ? m_wire->sendDesktop(bytes) : m_wire->sendFrame(bytes))) {
            if(m_peer->property("frameAck").toBool()) m_pendingFrames.enqueue({m_frameClock.elapsed(),int(bytes.size())});
            m_lastFrameSent=m_frameClock.elapsed();
            if(incremental) QMetaObject::invokeMethod(encoder,[encoder,generation]{encoder->commit(generation);},Qt::QueuedConnection);
            emit frameSent(bytes.size());
        }
        // Catch up to the selected cadence without occupying the input/network event loop.
        QTimer::singleShot(0,this,[this]{capture();});
    });
    m_encoderThread.start();
}
RemoteHost::~RemoteHost() { stop(); m_encoderThread.quit(); m_encoderThread.wait(); }
void RemoteHost::rotateCode() {
    disconnectPeer();
    const QString alphabet="23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    const QString old=m_code;
    do {
        m_code.clear();
        for (int i=0; i<8; ++i) m_code += alphabet.at(QRandomGenerator::system()->bounded(int(alphabet.size())));
    } while (m_code == old);
}
bool RemoteHost::setCode(const QString &code,QString *error) {
    if (code.size()<8 || code.size()>64 || code.contains(QChar::Null) || code.contains('\n') || code.contains('\r')) {
        if (error) *error="连接密码需为 8–64 个字符，不能包含换行。"; return false;
    }
    disconnectPeer(); m_code=code; return true;
}
QString RemoteHost::fingerprint() const { return certHash(m_cert); }
bool RemoteHost::loadIdentity(QString *error) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(dir)) { *error = "无法创建本地证书目录"; return false; }
    QFile::setPermissions(dir,QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString certPath = dir + "/identity.pem", keyPath = dir + "/identity.key";
    if (!QFile::exists(certPath) || !QFile::exists(keyPath)) {
        if (!Platform::generateIdentity(certPath,keyPath,error)) return false;
    }
    QFile certFile(certPath), keyFile(keyPath);
    if (!certFile.open(QIODevice::ReadOnly) || !keyFile.open(QIODevice::ReadOnly)) { *error = "读取本机证书失败"; return false; }
    keyFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    m_cert = QSslCertificate(certFile.readAll(),QSsl::Pem);
    m_key = QSslKey(keyFile.readAll(),QSsl::Rsa,QSsl::Pem);
    if (m_cert.isNull() || m_key.isNull() || m_cert.expiryDate() <= QDateTime::currentDateTimeUtc()) {
        *error = "本机证书无效或已过期，请备份并移除应用数据目录中的 identity.pem 和 identity.key 后重试。"; return false;
    }
    return true;
}
bool RemoteHost::start(quint16 port, QString *error, bool testMode, const QHostAddress &bind) {
    if (isListening()) return true;
    if (!QSslSocket::supportsSsl()) { *error = "Qt 的 TLS 后端不可用，请检查 Qt Network 插件部署。"; return false; }
    m_testMode = testMode;
    if (!testMode && !Platform::screenCaptureAvailable(error)) return false;
    if (!loadIdentity(error)) return false;
    if (!m_listener.listen(bind,port)) { *error = m_listener.errorString(); return false; }
    emit status("已开启共享，等待对方连接"); return true;
}
void RemoteHost::stop() {
    m_listener.close(); m_capture.stop(); m_heartbeat.stop();
    disconnectPeer();
    const auto sockets = m_sockets;
    for (auto s : sockets) s->abort();
    m_input.releaseAll();
}
void RemoteHost::disconnectPeer() { ++m_captureGeneration; m_encoding=false; if (m_peer) m_peer->abort(); m_input.releaseAll(); }
bool RemoteHost::setControl(bool enabled, QString *error) {
    if (enabled && !m_input.available(error)) return false;
    m_control = enabled;
    if (!enabled) m_input.releaseAll();
    if (m_wire) m_wire->sendJson({{"type","control"},{"enabled",enabled}});
    return true;
}
void RemoteHost::accept(qintptr descriptor) {
    auto socket = new QSslSocket(this);
    if (!socket->setSocketDescriptor(descriptor)) { socket->deleteLater(); return; }
    if (m_sockets.size() >= 4) { socket->abort(); socket->deleteLater(); return; }
    const auto ip = socket->peerAddress().toString();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (auto it=m_attempts.begin(); it!=m_attempts.end();) {
        if (now - it->since > 60000) it = m_attempts.erase(it); else ++it;
    }
    if (m_attempts.value(ip).count >= 5 || m_attempts.size() > 1024) { socket->abort(); socket->deleteLater(); return; }
    m_sockets.insert(socket);
    auto config = socket->sslConfiguration();
    config.setProtocol(QSsl::TlsV1_2OrLater);
    config.setLocalCertificate(m_cert); config.setPrivateKey(m_key);
    config.setPeerVerifyMode(QSslSocket::VerifyNone);
    socket->setSslConfiguration(config);
    socket->setReadBufferSize(Wire::MaxFrame + 65536);
    auto wire = new Wire(socket,socket);
    auto activity = new QElapsedTimer; activity->start();
    auto guard = new QTimer(socket); guard->setInterval(1000);
    connect(guard,&QTimer::timeout,socket,[socket,activity] {
        const bool auth = socket->property("authenticated").toBool();
        if (activity->elapsed() > (auth ? 90000 : 60000)) socket->abort();
    }); guard->start();
    connect(socket,&QSslSocket::disconnected,this,[this,socket,activity] {
        m_sockets.remove(socket); delete activity;
        if (m_peer == socket) {
            ++m_captureGeneration; m_encoding=false;
            m_peer.clear(); m_wire.clear(); m_capture.stop(); m_heartbeat.stop(); m_input.releaseAll();
            emit peerChanged(false,{}); emit status(isListening() ? "已开启共享，等待对方连接" : "共享已关闭");
        }
        socket->deleteLater();
    });
    connect(socket,&QSslSocket::errorOccurred,socket,[socket](QAbstractSocket::SocketError) { socket->abort(); });
    connect(wire,&Wire::invalidPacket,this,[this](const QString &s) { emit status(s); });
    connect(wire,&Wire::frameReceived,socket,[socket](const QByteArray &) { socket->abort(); });
    connect(wire,&Wire::jsonReceived,this,[this,socket,wire,ip,activity](const QJsonObject &j) {
        if (!socket->isEncrypted()) { socket->abort(); return; }
        if (socket->property("rejected").toBool()) return;
        const auto reject=[socket,wire](const QString &reason) {
            socket->setProperty("rejected",true);
            wire->sendJson({{"type","error"},{"message",reason}});
            // Let the peer consume the rejection and close. Bound peers that stay open.
            QTimer::singleShot(5000,socket,[socket] { socket->disconnectFromHost(); });
        };
        const auto type = j.value("type").toString();
        if (!socket->property("authenticated").toBool()) {
            if (type != "auth" || j.value("version").toInt() != 1) { socket->abort(); return; }
            if (!equalCode(j.value("code").toString(),m_code)) {
                auto &entry = m_attempts[ip];
                if (!entry.count) entry.since = QDateTime::currentMSecsSinceEpoch();
                ++entry.count;
                reject("验证码错误；连续 5 次失败将暂停该地址 60 秒。"); return;
            }
            if (m_peer) { reject("此设备已有远程会话，请稍后重试。"); return; }
            socket->setProperty("authenticated",true); activity->restart();
            m_peer = socket; m_wire = wire;
            ++m_captureGeneration; m_encoding=false; m_lastCapture=-10000;
            m_frameClock.start(); m_pendingFrames.clear(); m_stream=StreamSettings(); m_capture.setInterval(1000/m_stream.fps);
            socket->setProperty("desktopDelta",j.value("desktopDelta").toBool());
            m_lastFrameSent=-10000;
            socket->setProperty("frameAck",j.value("frameAck").toBool());
            socket->setProperty("streamOptions",j.value("streamOptions").toBool());
            auto screen = QGuiApplication::primaryScreen();
            m_geometry = screen ? Platform::screenGeometry(screen) : QRect(0,0,1280,720);
            wire->sendJson({{"type","welcome"},{"version",1},{"name",QHostInfo::localHostName()},
                {"control",m_control},{"streamOptions",true},{"width",m_geometry.width()},{"height",m_geometry.height()}});
            m_capture.start(); m_heartbeat.start(); capture();
            emit peerChanged(true,ip); emit status("远程设备已连接 · " + ip); return;
        }
        activity->restart();
        if (type == "ping") wire->sendJson({{"type","pong"}});
        else if (type == "frame_ack" && m_peer==socket && socket->property("frameAck").toBool() && !m_pendingFrames.isEmpty()) {
            m_pendingFrames.dequeue();
        }
        else if (type == "stream_settings" && m_peer==socket) {
            ++m_captureGeneration; m_encoding=false; m_lastCapture=-10000;
            m_stream=StreamSettings::fromJson(j);
            // Only peers advertising support can request PNG frames.
            m_stream.lossless=m_stream.lossless && socket->property("streamOptions").toBool();
            m_capture.setInterval(qMax(1,1000/m_stream.fps));
            wire->sendJson({{"type","stream_settings_applied"},{"settings",m_stream.json()}});
        }
        else if (type == "input" && m_peer == socket && m_control) m_input.apply(j,m_geometry);
    });
    socket->setSocketOption(QAbstractSocket::LowDelayOption,1);
    socket->startServerEncryption();
}
void RemoteHost::capture() {
    if (!m_peer || !m_wire || m_encoding || m_peer->bytesToWrite() > 256*1024) return;
    if(m_frameClock.elapsed()-m_lastCapture<qMax(1,1000/m_stream.fps)) return;
    const bool frameAck=m_peer->property("frameAck").toBool();
    // A bounded flight window prevents seconds of stale pictures hiding input responses.
    // It is not a bytes-per-second limiter; multiple frames still cover Internet RTT.
    if (frameAck && m_pendingFrames.size()>=(m_stream.lossless ? 4 : 8)) return;
    QImage image;
    if (m_testMode) {
        image = QImage(1280,720,QImage::Format_RGB32); image.fill(QColor("#101b32"));
        QPainter p(&image); p.setPen(Qt::white); p.setFont(QFont("sans-serif",32));
        p.drawText(image.rect(),Qt::AlignCenter,"LinkDesk · TLS integration test");
    } else {
        auto screen = QGuiApplication::primaryScreen(); if (!screen) return;
        m_geometry = Platform::screenGeometry(screen);
#ifndef Q_OS_WIN
        image = Platform::captureScreen(screen,QSize(m_stream.width,m_stream.height));
        if (image.isNull()) {
            m_wire->sendJson({{"type","error"},{"message","屏幕采集失败，请检查系统屏幕录制权限。"}});
            disconnectPeer(); return;
        }
#endif
    }
    // Keep the selected resolution and quality; congestion drops captures, never pixels.
    if (m_testMode && (image.width()>m_stream.width || image.height()>m_stream.height)) image=image.scaled(m_stream.width,m_stream.height,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    bool nativeCapture=false;
#ifdef Q_OS_WIN
    nativeCapture=!m_testMode;
#endif
    m_encoding=true; m_lastCapture=m_frameClock.elapsed();
    auto encoder=static_cast<CaptureEncoder *>(m_encoder); const auto generation=m_captureGeneration; const auto settings=m_stream;
    QMetaObject::invokeMethod(encoder,[encoder,generation,image,settings,nativeCapture,incremental=m_peer->property("desktopDelta").toBool()]{encoder->encode(generation,image,settings,nativeCapture,incremental);},Qt::QueuedConnection);
}

RemoteClient::RemoteClient(QObject *parent) : QObject(parent) {
    m_timeout.setSingleShot(true);
    connect(&m_timeout,&QTimer::timeout,this,[this] { emit error("连接超时，请检查地址、共享状态和防火墙。"); disconnectPeer(); });
    m_heartbeat.setInterval(3000);
    connect(&m_heartbeat,&QTimer::timeout,this,[this] { if (m_wire) m_wire->sendJson({{"type","ping"}}); });
}
void RemoteClient::connectTo(const QString &host, quint16 port, const QString &code,
                             const QString &identity, const QString &expectedFingerprint) {
    disconnectPeer(); m_code = code;
    auto socket = new QSslSocket(this); m_socket = socket;
    auto config = socket->sslConfiguration(); config.setProtocol(QSsl::TlsV1_2OrLater);
    config.setPeerVerifyMode(QSslSocket::VerifyPeer); socket->setSslConfiguration(config);
    socket->setReadBufferSize(Wire::MaxFrame + 65536);
    auto wire = new Wire(socket,socket); m_wire = wire;
    connect(socket,&QSslSocket::sslErrors,this,[this,socket](const QList<QSslError> &errors) {
        // Self-signed certificates are authenticated by a SHA-256 pin BEFORE sending the code.
        for (const auto &e : errors) {
            const bool localTrust = socket->peerCertificate().isSelfSigned() &&
                (e.error() == QSslError::SelfSignedCertificate || e.error() == QSslError::CertificateUntrusted);
            if (!localTrust && e.error() != QSslError::HostNameMismatch) {
                emit error("TLS 证书校验失败："+e.errorString()); socket->abort(); return;
            }
        }
        socket->ignoreSslErrors(errors);
    });
    connect(socket,&QSslSocket::encrypted,this,[this,socket,host,port,identity,expectedFingerprint] {
        if (m_socket != socket) return;
        m_timeout.start(60000); m_trusting = true;
        if (!expectedFingerprint.isEmpty()) {
            QString actual=certHash(socket->peerCertificate()).remove(':').toLower();
            QString expected=QString(expectedFingerprint).remove(':').toLower();
            if (actual != expected) { emit error("被控端证书与设备注册信息不一致，连接已拒绝。"); disconnectPeer(); return; }
            trustCertificate(true);
        } else emit certificateCheck(identity.isEmpty() ? host.toLower()+":"+QString::number(port) : identity,certHash(socket->peerCertificate()));
    });
    connect(socket,&QSslSocket::errorOccurred,this,[this,socket](QAbstractSocket::SocketError e) {
        if (m_socket == socket) {
            if (e != QAbstractSocket::RemoteHostClosedError) emit error(socket->errorString());
            disconnectPeer();
        }
    });
    connect(socket,&QSslSocket::disconnected,this,[this,socket] {
        if (m_socket == socket) {
            m_active=false; m_trusting=false; m_code.clear(); m_timeout.stop(); m_heartbeat.stop();
            m_socket.clear(); m_wire.clear(); emit disconnected();
        } socket->deleteLater();
    });
    connect(wire,&Wire::invalidPacket,this,&RemoteClient::error);
    auto invalidImage=[this] { emit error("远程画面格式无效。"); disconnectPeer(); };
    connect(wire,&Wire::frameReceived,this,[this,socket,invalidImage](const QByteArray &bytes) {
        if(m_socket!=socket || !m_active) return;
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer); reader.setDecideFormatFromContent(true);
        const auto size=reader.size(); if(!size.isValid() || size.width()>4096 || size.height()>4096 || qint64(size.width())*size.height()>8000000) { invalidImage(); return; }
        const auto image=reader.read(); if(image.isNull()) { invalidImage(); return; }
        m_timeout.start(90000); emit frame(bytes); emit imageFrame(image,bytes.size()+5,true);
        if(m_wire) m_wire->sendJson({{"type","frame_ack"}});
    });
    connect(wire,&Wire::desktopReceived,this,[this,socket](const QByteArray &bytes) {
        if(m_socket!=socket || !m_active) return;
        QImage image; bool changed=false; QString message;
        if(!m_desktopDecoder.decode(bytes,&image,&changed,&message)) { emit error(message); disconnectPeer(); return; }
        m_timeout.start(90000); emit imageFrame(image,bytes.size()+5,changed);
        if(m_wire) m_wire->sendJson({{"type","frame_ack"}});
    });
    connect(wire,&Wire::jsonReceived,this,[this,socket,wire](const QJsonObject &j) {
        if (m_socket != socket) return;
        const auto type = j.value("type").toString();
        if (type == "welcome" && !m_trusting && j.value("version").toInt() == 1 && !m_active) {
            m_active = true; m_streamSupported=j.value("streamOptions").toBool(); m_timeout.start(90000); m_heartbeat.start();
            setStreamSettings(m_stream);
            emit connected(j.value("name").toString(),j.value("control").toBool());
        } else if (type == "error") { emit error(j.value("message").toString()); disconnectPeer(); }
        else if (m_active) {
            m_timeout.start(90000);
            if (type == "control") emit controlChanged(j.value("enabled").toBool());
            else if (type == "stream_settings_applied") emit streamSettingsApplied(j.value("settings").toObject());
            else if (type == "ping") wire->sendJson({{"type","pong"}});
        }
    });
    socket->setSocketOption(QAbstractSocket::LowDelayOption,1);
    m_timeout.start(45000); socket->connectToHostEncrypted(host,port);
}
void RemoteClient::trustCertificate(bool accepted) {
    if (!m_trusting || !m_socket || !m_wire) return;
    m_trusting=false;
    if (!accepted) { disconnectPeer(); return; }
    m_wire->sendJson({{"type","auth"},{"version",1},{"code",m_code},{"frameAck",true},{"streamOptions",true},{"desktopDelta",!property("legacyFrames").toBool()}});
    m_code.clear(); m_timeout.start(45000);
}
void RemoteClient::setStreamSettings(const StreamSettings &settings) {
    m_stream=StreamSettings::fromJson(settings.json());
    if (!m_active || !m_wire || !m_streamSupported) return;
    auto j=m_stream.json(); j["type"]="stream_settings"; m_wire->sendJson(j);
}
void RemoteClient::disconnectPeer() {
    m_desktopDecoder.reset();
    m_timeout.stop(); m_heartbeat.stop(); m_trusting=false; m_code.clear();
    if (m_socket) {
        auto socket = m_socket; m_socket.clear(); m_wire.clear(); m_active=false;
        socket->abort(); socket->deleteLater(); emit disconnected();
    }
}
void RemoteClient::sendInput(const QJsonObject &event) {
    if (!m_active || !m_wire) return;
    auto j = event; j["type"]="input"; m_wire->sendJson(j);
}
#include "remote.moc"
