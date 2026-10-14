#include "relay.h"
#include "remote.h"
#include "platform.h"
#include "viewer.h"
#include "protocol.h"
#include <QTest>
#include <QMouseEvent>
#include <QToolBar>
#include <QApplication>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QScreen>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QStandardPaths>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QImage>

class ProbeEdit : public QLineEdit {
protected:
    void mousePressEvent(QMouseEvent *event) override { qInfo("Native mouse click reached test input"); QLineEdit::mousePressEvent(event); }
};

// Manual cross-platform smoke probe. Credentials are exchanged through a private
// local descriptor file; neither passwords nor enrollment keys are logged.
int main(int argc,char **argv) {
    QApplication app(argc,argv); app.setOrganizationName("LinkDeskTests"); app.setApplicationName("LinkDeskProbe");
    QStandardPaths::setTestModeEnabled(true); Platform::initialize();
    auto args=app.arguments(); if (args.size()!=4 && args.size()!=5) return 2;
    const bool direct=qEnvironmentVariableIsSet("LINKDESK_PROBE_DIRECT");
    const bool forceRelay=qEnvironmentVariableIsSet("LINKDESK_PROBE_RELAY");
    const bool hosting=args[1]=="host"; if (!hosting && args[1]!="viewer") return 2;
    QFile file(args[2]); if (!file.open(QIODevice::ReadOnly)) return 2;
    auto json=QJsonDocument::fromJson(file.readAll()).object(); file.close(); RelayConfig config;
    config.host=json["server"].toString(); config.port=quint16(json["port"].toInt(443));
    config.fingerprint=json["fingerprint"].toString(); config.enrollmentKey=json["enrollmentKey"].toString();
    QString error; if (!config.valid(&error)) { qCritical("Invalid server config"); return 2; }
    QTemporaryDir dir; if (!dir.isValid()) return 2;
    const bool managed=qEnvironmentVariableIsSet("LINKDESK_PROBE_MANAGED");
    const int requestedFps=qEnvironmentVariableIntValue("LINKDESK_PROBE_FPS");
    if(requestedFps>0) { auto s=StreamSettings(); s.fps=qBound(1,requestedFps,30); QSettings().setValue("viewer/streamSettings",s.json()); }
    RelayAgent relay; RemoteHost host; RemoteClient client; ViewerWindow viewer(&client);
    if(managed) { viewer.setManaged(true); viewer.preview(); }
    if (qEnvironmentVariableIsSet("LINKDESK_PROBE_LOSSLESS")) {
        auto settings=client.streamSettings(); settings.lossless=true; client.setStreamSettings(settings);
    }
    QLabel window("LinkDesk Windows / Mac interoperability verification\nReal desktop capture through the public TLS relay");
    window.setWindowTitle("LinkDesk interoperability test"); window.resize(700,200);
    window.setStyleSheet("background:#172b4d;color:white;font-size:20px;padding:24px;");
    ProbeEdit edit; edit.setWindowTitle("LinkDesk public remote input verification"); edit.resize(700,150);
    edit.setAttribute(Qt::WA_InputMethodEnabled,false); edit.setInputMethodHints(Qt::ImhLatinOnly);
    int frames=0,updates=0; qint64 receivedBytes=0; QElapsedTimer measure; bool sentInput=false, expanded=false; QTimer telemetry; telemetry.setInterval(5000);
    QObject::connect(&telemetry,&QTimer::timeout,&app,[&]{ auto stats=viewer.findChild<QLabel *>("liveSessionStats"); if (stats) qInfo().noquote()<<"Viewer:"<<stats->text(); if (args.size()==5 && frames>0) viewer.findChild<QToolBar *>()->grab().save(args[4]+".toolbar.png"); });
    QTimer deadline; deadline.setSingleShot(true); deadline.start(600000);
    QObject::connect(&deadline,&QTimer::timeout,&app,[&]{ qCritical("Probe timed out"); app.exit(1); });
    QObject::connect(&relay,&RelayAgent::error,&app,[&](const QString &message){ qCritical().noquote()<<message; app.exit(1); });
    QObject::connect(&client,&RemoteClient::error,&app,[&](const QString &message){ qCritical().noquote()<<message; app.exit(1); });
    QTimer animation; animation.setInterval(33); int phase=0;
    bool hadPeer=false;
    if (hosting) {
        QObject::connect(&host,&RemoteHost::peerChanged,&app,[&](bool connected,const QString &){
            if(connected) {
                hadPeer=true;
                for(auto wire:host.findChildren<Wire *>()) QObject::connect(wire,&Wire::jsonReceived,&app,[](const QJsonObject &event){
                    if(event.value("type")=="input") qInfo().noquote()<<"Host input delivered:"<<event.value("action").toString();
                });
            } else if(hadPeer) app.quit();
        });
        edit.show(); edit.raise(); edit.activateWindow();
#ifdef Q_OS_WIN
        QTimer::singleShot(800,&edit,[&]{ auto english=LoadKeyboardLayoutW(L"00000409",KLF_NOTELLSHELL); ActivateKeyboardLayout(english,0); SendMessageW(reinterpret_cast<HWND>(edit.winId()),WM_INPUTLANGCHANGEREQUEST,0,reinterpret_cast<LPARAM>(english)); });
#endif
        QObject::connect(&animation,&QTimer::timeout,&edit,[&] { ++phase; edit.setStyleSheet(QString("background:rgb(%1,230,240);color:#102030;font-size:24px;").arg(190+phase%40)); });
        if(qEnvironmentVariableIsSet("LINKDESK_PROBE_ANIMATE")) QTimer::singleShot(17000,&edit,[&] { animation.start(); qInfo("Native small-region animation started: target 30 Hz"); });
        QObject::connect(&edit,&QLineEdit::textChanged,&app,[&](const QString &text){ if (text=="ab") qInfo("Public remote keyboard verified: ab"); });
        if (!host.start(0,&error,false,QHostAddress::Any)) { qCritical().noquote()<<error; return 1; }
        if (!host.setControl(true,&error)) { qCritical().noquote()<<error; return 1; }
        relay.setAvailable(host.port(),host.fingerprint());
        auto publish=[&](bool online,const QString &id){
            if (!online) return;
            QSaveFile descriptor(args[3]); if (!descriptor.open(QIODevice::WriteOnly)) { app.exit(1); return; }
            descriptor.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
            const auto logical=edit.screen()->geometry();
            auto point=edit.mapToGlobal(edit.rect().center());
            const double x=double(point.x()-logical.x())/logical.width(),y=double(point.y()-logical.y())/logical.height();
            descriptor.write(QJsonDocument(QJsonObject{{"id",id},{"password",host.code()},{"x",x},{"y",y},{"port",int(host.port())},{"fingerprint",host.fingerprint()}}).toJson());
            if (!descriptor.commit()) { app.exit(1); return; } qInfo("Host ready");
        };
        QObject::connect(&relay,&RelayAgent::onlineChanged,&app,publish);
        if (direct) publish(true,"000000001");
    } else {
        QFile descriptor(args[3]); if (!descriptor.open(QIODevice::ReadOnly)) return 2;
        auto target=QJsonDocument::fromJson(descriptor.readAll()).object();
        const QString id=target["id"].toString(),password=target["password"].toString();
        const double x=target["x"].toDouble(),y=target["y"].toDouble();
        QObject::connect(&client,&RemoteClient::disconnected,&app,[&]{ qCritical("Unexpected remote disconnect"); app.exit(1); });
        QObject::connect(&client,&RemoteClient::connected,&app,[&](const QString &name,bool control){
            if(!control) { app.exit(1); return; }
            auto open=[&,name,control] { viewer.begin(name,control); measure.start(); expanded=true; if(managed) viewer.showFullScreen(); telemetry.start(); };
            if(managed) QTimer::singleShot(2000,&app,open); else open();
            const int duration=qMax(22000,qEnvironmentVariableIntValue("LINKDESK_PROBE_DURATION_MS"));
            QTimer::singleShot(duration,&app,[&]{ if (frames<15 || (!direct && !relay.online()) || !client.active()) { qCritical("Session verification failed: frames=%d online=%d active=%d",frames,relay.online(),client.active()); app.exit(1); return; } qInfo("Session stable, frames=%d updates=%d direct=%d average=%.1f KB/s",frames,updates,direct,double(receivedBytes)/qMax<qint64>(1,measure.elapsed())); app.exit(0); });
        });
        if (direct) QTimer::singleShot(0,&app,[&,password,target]{ client.connectTo("192.168.100.15",quint16(target["port"].toInt()),password,"LAN verification",target["fingerprint"].toString()); });
        QObject::connect(&relay,&RelayAgent::onlineChanged,&app,[&relay,id,forceRelay](bool online,const QString &){ if (online) { if (forceRelay) relay.connectViaRelay(id); else relay.connectDevice(id); } });
        QObject::connect(&relay,&RelayAgent::directReady,&client,[&client,password](const QString &host,quint16 port,const QString &id,const QString &fp){ qInfo("LAN peer selected automatically"); client.connectTo(host,port,password,id,fp); });
        QObject::connect(&relay,&RelayAgent::viewerReady,&client,[&client,password](quint16 port,const QString &id,const QString &fp){ client.connectTo("127.0.0.1",port,password,id,fp); });
        QObject::connect(&client,&RemoteClient::imageFrame,&app,[&,x,y](const QImage &image,qint64 wireBytes,bool changed){
            if(managed && (!expanded || image.width()<=480)) return; // Only measure the expanded desktop.
            if (args.size()==5 && frames==0 && !image.save(args[4])) { app.exit(1); return; }
            ++frames; receivedBytes+=wireBytes; if(changed) ++updates;
            if (!sentInput) { sentInput=true; qInfo("Cross-platform real frame received: %d x %d",image.width(),image.height());
                auto canvas=viewer.findChild<RemoteCanvas *>(); if (!canvas) { app.exit(1); return; }
                QTimer::singleShot(1000,&app,[&,canvas,x,y]{
                    if(!canvas->controlEnabled()) { qCritical("Expanded canvas lost input permission"); app.exit(1); return; }
                    const auto fit=canvas->property("encodedFrameSize").toSize().scaled(canvas->size(),Qt::KeepAspectRatio);
                    const QPoint point(qRound((canvas->width()-fit.width())/2.0+x*fit.width()),qRound((canvas->height()-fit.height())/2.0+y*fit.height()));
                    qInfo()<<"Input geometry:"<<canvas->size()<<fit<<point<<x<<y;
                    QSignalSpy inputs(canvas,&RemoteCanvas::input);
                    QTest::mouseMove(canvas,point); QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,point); QTest::qWait(100);
                    if(inputs.size()<2) { qCritical("Canvas failed to emit mouse input"); app.exit(1); return; }
                    qInfo("Viewer canvas mouse events sent");
                    QTimer::singleShot(500,&app,[canvas]{ QTest::keyClick(canvas,Qt::Key_A); QTest::keyClick(canvas,Qt::Key_B); });
                });
            }
        });
    }
    if (!direct) relay.start(config,dir.filePath("probe-identity.json"));
    return app.exec();
}
