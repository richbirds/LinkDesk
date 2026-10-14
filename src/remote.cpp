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
#include <QNetworkProxy>

QJsonObject StreamSettings::json() const {
    return {{"fps",fps},{"quality",quality},{"width",width},{"height",height},{"lossless",lossless},{"codec",codec}};
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
    const auto codec=j.value("codec").toString("auto"); if(codec=="desktop" || codec=="h264") s.codec=codec;
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
    ~CaptureEncoder() override { suspend(); }
    DesktopEncoder desktop; quint64 currentGeneration=~quint64(0);
    std::unique_ptr<VideoEncoder> video;
    QImage committed,pending; bool videoActive=false,nativeFailed=false;
    quint64 committedSerial=0,pendingSerial=0;
    bool nativeCapturing=false;
    int motionStreak=0;
    QImage observed;
    void suspend() { if(nativeCapturing) Platform::stopCapture();nativeCapturing=false;if(video) video->reset(); }
    quint32 videoSequence=0; qint64 lastChange=0;
    void commit(quint64 generation,bool encodedVideo) {
        if(generation!=currentGeneration) return;
        committed=pending;committedSerial=pendingSerial; videoActive=encodedVideo;
        if(encodedVideo) desktop.reset(); else { desktop.commit(); if(video) video->reset(); videoSequence=0; }
    }
    void discard(quint64 generation) { if(generation==currentGeneration && video) { video->reset(); videoSequence=0; } }
    void encode(quint64 generation,QImage image,const StreamSettings &settings,bool nativeCapture,bool incremental,bool office,bool allowVideo,qint64 capturedAt,int targetBitrate) {
        QElapsedTimer timing; timing.start();
        if(generation!=currentGeneration) { desktop.reset(); video.reset();committed={}; pending={};observed={};motionStreak=0;committedSerial=pendingSerial=0;videoActive=false;nativeFailed=false;videoSequence=0;lastChange=capturedAt; currentGeneration=generation; }
        desktop.setOfficeOptimizations(office && settings.quality>30);
        CaptureFrame frame;
        nativeCapturing=nativeCapture;
        if(nativeCapture) frame=Platform::captureFrame(nullptr,QSize(settings.width,settings.height)); else frame.image=image;
        image=frame.image;
        timing.restart(); // Keep native acquisition time out of encoding metrics.
        auto finish=[&](const QByteArray &bytes,const QString &error,bool encodedVideo=false) { emit ready(generation,bytes,error,timing.nsecsElapsed()/1000,capturedAt,encodedVideo); };
        if(frame.pending) { finish({},{});return; }
        if(image.isNull()) { finish({},"屏幕采集失败，请检查系统屏幕录制权限。"); return; }
        if(office && !videoActive && frame.unchanged && frame.serial && frame.serial==committedSerial && !desktop.needsRefinement()) { finish({},{});return; }
        if(office && videoActive && committed==image) {
            if(capturedAt-lastChange<600) { finish({},{}); return; }
            pending=image;pendingSerial=frame.serial; frame.image=image; const auto bytes=desktop.encode(frame,settings.quality,true,capturedAt); finish(bytes,bytes.isEmpty() ? "文字修复编码失败。" : QString()); return;
        }
        const bool sourceChanged=committed!=image;
        if(observed!=image) { observed=image;lastChange=capturedAt;motionStreak=qMin(3,motionStreak+1); }
        else if(capturedAt-lastChange>=600) motionStreak=0;
        pending=image;pendingSerial=frame.serial;
        if(incremental) {
            const bool videoCapable=allowVideo && !nativeFailed && !settings.lossless && settings.quality>30 && settings.codec!="desktop" && !(image.width()&1) && !(image.height()&1);
            const bool continuingVideo=videoCapable && videoActive && sourceChanged;
            QByteArray bytes;if(!continuingVideo) bytes=desktop.encode(frame,settings.quality,settings.lossless,capturedAt);
            const bool motion=!continuingVideo && desktop.changedRatio()>.25 && bytes.size()>48*1024 && motionStreak>=3;
            if(videoCapable && sourceChanged && (motion || continuingVideo || settings.codec=="h264")) {
                if(!video) video=createVideoEncoder();
                VideoSettings options{image.size(),settings.fps,targetBitrate>0 ? targetBitrate : qMax(1000000,image.width()*image.height()*settings.fps/15)};
                QString nativeError; const auto encoded=video->encode(image,options,&nativeError);
                if(!encoded.isEmpty()) { const auto packet=videoPacket(encoded,image.size(),++videoSequence); if(!packet.isEmpty()) { finish(packet,{},true); return; } }
                nativeFailed=true; video->reset(); videoSequence=0;
                emit backendFallback(generation,nativeError.isEmpty() ? "硬件视频编码失败，已回退增量图像。" : nativeError);
            }
            if(bytes.isEmpty()) bytes=desktop.encode(frame,settings.quality,settings.lossless,capturedAt);
            finish(bytes,bytes.isEmpty() ? "增量画面编码失败。" : QString()); return;
        }
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
        if(!image.save(&buffer,settings.lossless ? "PNG" : "JPEG",settings.lossless ? -1 : settings.quality)) {
            finish({},"画面编码失败。"); return;
        }
        finish(bytes,{});
    }
signals:
    void ready(quint64 generation,const QByteArray &bytes,const QString &error,qint64 encodeUs,qint64 capturedAt,bool video);
    void backendFallback(quint64 generation,const QString &reason);
};
RemoteHost::RemoteHost(QObject *parent) : QObject(parent) {
    rotateCode();
    m_capture.setInterval(100);
    m_capture.setTimerType(Qt::PreciseTimer);
    m_heartbeat.setInterval(3000);
    connect(&m_listener,&Listener::accepted,this,&RemoteHost::accept);
    connect(&m_capture,&QTimer::timeout,this,&RemoteHost::capture);
    connect(&m_heartbeat,&QTimer::timeout,this,[this] {
        if (m_wire) m_wire->sendJson({{"type","ping"}});
    });
    auto encoder=new CaptureEncoder; m_encoder=encoder; encoder->moveToThread(&m_encoderThread);
    connect(&m_encoderThread,&QThread::finished,encoder,&QObject::deleteLater);
    connect(encoder,&CaptureEncoder::backendFallback,this,[this](quint64 generation,const QString &reason) { if(generation==m_captureGeneration) { ++m_videoFallbacks;emit status(reason); } });
    connect(encoder,&CaptureEncoder::ready,this,[this,encoder](quint64 generation,const QByteArray &bytes,const QString &error,qint64 encodeUs,qint64 capturedAt,bool video) {
        if(generation!=m_captureGeneration) return;
        m_encoding=false; if(!m_peer || !m_wire) return;
        if(!error.isEmpty()) { m_wire->sendJson({{"type","error"},{"message",error}}); disconnectPeer(); return; }
        if(bytes.isEmpty()) return;
        if(bytes.size()>Wire::MaxFrame) { m_wire->sendJson({{"type","error"},{"message","无损画面过大，请降低分辨率或选择智能增量模式。"}}); disconnectPeer(); return; }
        const bool incremental=m_peer->property("desktopDelta").toBool();
        // Keep the displayed reference alive without retransmitting its pixels.
        // A repeat is only 19 bytes, and lets older viewers distinguish a static
        // desktop from a stalled capture/decoder pipeline.
        if(incremental && !video && bytes.size()==19 && m_frameClock.elapsed()-m_lastFrameSent<3000) return;
        qint64 inFlight=0; for(const auto &pending:m_pendingFrames) inFlight+=pending.second;
        const qint64 limit=m_stream.lossless ? qint64(Wire::MaxFrame)*2 : 8*1024*1024;
        bool sent=false; const auto id=++m_frameId;
        if(bytes.size()<=Wire::MaxFrame && inFlight+bytes.size()<=limit && m_wire->visualWritable() && m_scheduler.canCapture(m_frameClock.elapsed(),m_peer->bytesToWrite())) {
            if(m_peer->property("frameMetrics").toBool()) m_wire->sendJson({{"type","frame_info"},{"id",double(id)}});
            sent=video ? m_wire->sendVideo(bytes) : incremental ? m_wire->sendDesktop(bytes) : m_wire->sendFrame(bytes);
        }
        if(sent) {
            if(video) ++m_videoFrames;
            if(m_peer->property("frameAck").toBool()) m_pendingFrames.enqueue({m_frameClock.elapsed(),int(bytes.size())});
            m_lastFrameSent=m_frameClock.elapsed();
            if(m_peer->property("frameAck").toBool()) m_scheduler.sent(id,m_lastFrameSent,bytes.size()+5,capturedAt,encodeUs);
            if(incremental) QMetaObject::invokeMethod(encoder,[encoder,generation,video]{encoder->commit(generation,video);},Qt::QueuedConnection);
            emit frameSent(bytes.size()+5);
        } else {
            QMetaObject::invokeMethod(encoder,[encoder,generation]{encoder->discard(generation);},Qt::QueuedConnection);
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
void RemoteHost::disconnectPeer() { ++m_captureGeneration; m_encoding=false; if (m_peer) m_peer->abort(); m_input.releaseAll();if(m_encoder) { auto encoder=static_cast<CaptureEncoder *>(m_encoder);QMetaObject::invokeMethod(encoder,[encoder]{encoder->suspend();},Qt::QueuedConnection); } }
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
            auto encoder=static_cast<CaptureEncoder *>(m_encoder);QMetaObject::invokeMethod(encoder,[encoder]{encoder->suspend();},Qt::QueuedConnection);
            emit peerChanged(false,{}); emit status(isListening() ? "已开启共享，等待对方连接" : "共享已关闭");
        }
        socket->deleteLater();
    });
    connect(socket,&QSslSocket::errorOccurred,socket,[socket](QAbstractSocket::SocketError) { socket->abort(); });
    connect(socket,&QSslSocket::encryptedBytesWritten,this,[this,socket](qint64 bytes) { if(m_peer==socket) m_transportBytes+=bytes; });
    connect(wire,&Wire::invalidPacket,this,[this](const QString &s) { emit status(s); });
    connect(wire,&Wire::frameReceived,socket,[socket](const QByteArray &) { socket->abort(); });
    connect(wire,&Wire::videoReceived,socket,[socket](const QByteArray &) { socket->abort(); });
    connect(wire,&Wire::desktopReceived,socket,[socket](const QByteArray &) { socket->abort(); });
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
            m_frameClock.start(); m_pendingFrames.clear(); m_scheduler.reset();m_transportBytes=0;m_frameId=0;m_lastMetrics=0;m_cursorShape.clear();m_lastCursor={}; m_stream=StreamSettings(); m_capture.setInterval(1000/m_stream.fps);
            m_videoFrames=0;m_videoFallbacks=0;
            socket->setProperty("desktopDelta",j.value("desktopDelta").toBool());
            m_lastFrameSent=-10000;
            socket->setProperty("frameAck",j.value("frameAck").toBool());
            socket->setProperty("streamOptions",j.value("streamOptions").toBool());
            socket->setProperty("officeV2",j.value("officeV2").toBool());
            socket->setProperty("cursorChannel",j.value("cursorChannel").toBool());
            socket->setProperty("videoH264",j.value("videoH264").toBool());
            socket->setProperty("frameMetrics",j.value("frameMetrics").toBool());
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
            const auto id=socket->property("frameMetrics").toBool() ? quint64(j.value("id").toDouble()) : 0;
            if((!socket->property("frameMetrics").toBool() || id) && m_scheduler.acknowledge(id,m_frameClock.elapsed(),j.value("decodeMs").toDouble())) m_pendingFrames.dequeue();
        }
        else if(type=="video_error" && m_peer==socket) { socket->setProperty("videoH264",false);++m_videoFallbacks;++m_captureGeneration;m_encoding=false;m_lastCapture=-10000; }
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
    const auto now=m_frameClock.elapsed();
    if(now-m_lastMetrics>=1000) {
        auto metrics=streamingMetrics();metrics["socketQueueBytes"]=double(m_peer->bytesToWrite());
        emit streamMetrics(metrics);if(m_peer->property("frameMetrics").toBool()) { auto message=metrics;message["type"]="stream_metrics";m_wire->sendJson(message); }m_lastMetrics=now;
    }
    if(!m_testMode && m_stream.width>480 && m_stream.quality>30 && m_peer->property("cursorChannel").toBool()) {
        auto cursor=Platform::captureCursor(m_geometry);
        if(!cursor.image.isNull() && !m_geometry.isEmpty()) {
            const auto encoded=m_geometry.size().scaled(QSize(m_stream.width,m_stream.height).boundedTo(m_geometry.size()),Qt::KeepAspectRatio);
            const double scale=double(encoded.width())/m_geometry.width();cursor.image=cursor.image.scaled(qMax(1,int(cursor.image.width()*scale)),qMax(1,int(cursor.image.height()*scale)),Qt::KeepAspectRatio,Qt::SmoothTransformation);cursor.hotspot=QPoint(int(cursor.hotspot.x()*scale),int(cursor.hotspot.y()*scale));
        }
        QJsonObject message{{"type","cursor"},{"x",cursor.position.x()},{"y",cursor.position.y()},{"visible",cursor.visible}};
        QByteArray shape;
        if(!cursor.image.isNull()) { QBuffer buffer(&shape);buffer.open(QIODevice::WriteOnly);cursor.image.save(&buffer,"PNG"); }
        if(shape!=m_cursorShape && shape.size()<10000) { message["image"]=QString::fromLatin1(shape.toBase64());message["hx"]=cursor.hotspot.x();message["hy"]=cursor.hotspot.y(); }
        if(message!=m_lastCursor) { m_wire->sendJson(message);m_lastCursor=message;m_cursorShape=shape; }
    }
    if(!m_scheduler.canCapture(now,m_peer->bytesToWrite())) return;
    if(m_frameClock.elapsed()-m_lastCapture<qMax(1,1000/m_stream.fps)-1) return;
    const bool frameAck=m_peer->property("frameAck").toBool();
    // A bounded flight window prevents seconds of stale pictures hiding input responses.
    // It is not a bytes-per-second limiter; multiple frames still cover Internet RTT.
    if (frameAck && m_pendingFrames.size()>=(m_stream.lossless ? 4 : 8)) return;
    QImage image;
    if (m_testMode) {
        if(!m_syntheticFrame.isNull()) image=m_syntheticFrame;
        else {
        image = QImage(1280,720,QImage::Format_RGB32); image.fill(QColor("#101b32"));
        QPainter p(&image); p.setPen(Qt::white); p.setFont(QFont("sans-serif",32));
        p.drawText(image.rect(),Qt::AlignCenter,"LinkDesk · TLS integration test");
        }
    } else {
        auto screen = QGuiApplication::primaryScreen(); if (!screen) return;
        m_geometry = Platform::screenGeometry(screen);
    }
    // Keep the selected resolution and quality; congestion drops captures, never pixels.
    if (m_testMode && (image.width()>m_stream.width || image.height()>m_stream.height)) image=image.scaled(m_stream.width,m_stream.height,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    bool nativeCapture=!m_testMode;
    m_encoding=true; m_lastCapture=m_frameClock.elapsed();
    auto encoder=static_cast<CaptureEncoder *>(m_encoder); const auto generation=m_captureGeneration; const auto settings=m_stream;
    const bool incremental=m_peer->property("desktopDelta").toBool(),office=m_peer->property("officeV2").toBool(),video=m_peer->property("videoH264").toBool();
    const int bitrate=int(qMin<qint64>(2147483647,m_scheduler.estimatedBitrate()));
    QMetaObject::invokeMethod(encoder,[encoder,generation,image,settings,nativeCapture,incremental,office,video,now,bitrate]{encoder->encode(generation,image,settings,nativeCapture,incremental,office,video,now,bitrate);},Qt::QueuedConnection);
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
    // This is a direct peer or the local endpoint of an already verified relay.
    socket->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
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
        QElapsedTimer decode;decode.start();
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer); reader.setDecideFormatFromContent(true);
        const auto size=reader.size(); if(!size.isValid() || size.width()>4096 || size.height()>4096 || qint64(size.width())*size.height()>8000000) { invalidImage(); return; }
        const auto image=reader.read(); if(image.isNull()) { invalidImage(); return; }
        m_timeout.start(90000); emit frame(bytes); emit imageFrame(image,bytes.size()+5,true);
        if(m_wire) m_wire->sendJson({{"type","frame_ack"},{"id",double(m_ackId)},{"decodeMs",decode.nsecsElapsed()/1000000.0}});
    });
    connect(wire,&Wire::desktopReceived,this,[this,socket](const QByteArray &bytes) {
        if(m_socket!=socket || !m_active) return;
        QElapsedTimer decode;decode.start();
        QImage image; bool changed=false; QString message;
        if(!m_desktopDecoder.decode(bytes,&image,&changed,&message)) { emit error(message); disconnectPeer(); return; }
        m_timeout.start(90000); emit imageFrame(image,bytes.size()+5,changed);
        if(m_wire) m_wire->sendJson({{"type","frame_ack"},{"id",double(m_ackId)},{"decodeMs",decode.nsecsElapsed()/1000000.0}});
    });
    connect(wire,&Wire::videoReceived,this,[this,socket](const QByteArray &bytes) {
        if(m_socket!=socket || !m_active) return;
        QElapsedTimer decode;decode.start();QByteArray data;QSize size;quint32 sequence=0;QString message;
        if(!unpackVideoPacket(bytes,&data,&size,&sequence,&message)) { emit error(message);disconnectPeer();return; }
        if(!m_videoDecoder) m_videoDecoder=createVideoDecoder();
        if(sequence==1) { m_videoDecoder->reset();m_videoSequence=0; }
        QImage image;
        if(sequence==m_videoSequence+1) image=m_videoDecoder->decode(data,size,&message);
        m_videoSequence=sequence;
        if(image.isNull()) { if(m_wire) m_wire->sendJson({{"type","video_error"}});m_videoDecoder->reset(); }
        else { m_timeout.start(90000);emit imageFrame(image,bytes.size()+5,true); }
        if(m_wire) m_wire->sendJson({{"type","frame_ack"},{"id",double(m_ackId)},{"decodeMs",decode.nsecsElapsed()/1000000.0}});
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
            else if(type=="frame_info") m_ackId=quint64(j.value("id").toDouble());
            else if(type=="stream_metrics") emit streamMetrics(j);
            else if(type=="cursor") {
                if(!j.value("visible").toBool() && j.value("image").toString().isEmpty()) { emit cursorFrame(m_cursorImage,m_cursorHotspot,{},false);return; }
                if(j.contains("image")) {
                    const auto data=QByteArray::fromBase64(j.value("image").toString().toLatin1()); QBuffer buffer;buffer.setData(data);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer,"PNG");
                    if(!reader.size().isValid() || reader.size().width()>128 || reader.size().height()>128) return;
                    const auto image=reader.read();if(image.isNull()) return;
                    const QPoint hotspot(j.value("hx").toInt(),j.value("hy").toInt());if(!image.rect().contains(hotspot)) return;
                    m_cursorImage=image;m_cursorHotspot=hotspot;
                }
                const double x=j.value("x").toDouble(),y=j.value("y").toDouble();
                emit cursorFrame(m_cursorImage,m_cursorHotspot,QPointF(qBound(0.0,x,1.0),qBound(0.0,y,1.0)),j.value("visible").toBool());
            }
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
    const bool incremental=!property("legacyFrames").toBool();
    m_wire->sendJson({{"type","auth"},{"version",1},{"code",m_code},{"frameAck",true},{"streamOptions",true},{"desktopDelta",incremental},
        {"officeV2",incremental && !property("legacyOffice").toBool()},{"cursorChannel",true},{"frameMetrics",true},{"videoH264",incremental && !property("disableVideo").toBool()}});
    m_code.clear(); m_timeout.start(45000);
}
void RemoteClient::setStreamSettings(const StreamSettings &settings) {
    m_stream=StreamSettings::fromJson(settings.json());
    if (!m_active || !m_wire || !m_streamSupported) return;
    auto j=m_stream.json(); j["type"]="stream_settings"; m_wire->sendJson(j);
}
void RemoteClient::disconnectPeer() {
    m_desktopDecoder.reset();
    if(m_videoDecoder) m_videoDecoder->reset();m_videoSequence=0;m_ackId=0;m_cursorImage={};
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
