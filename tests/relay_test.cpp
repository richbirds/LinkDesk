#include "relay.h"
#include "remote.h"
#include "platform.h"
#include "connectionlist.h"
#include "viewer.h"
#include <QJsonArray>
#include <QSettings>
#include <memory>
#include <vector>
#include <algorithm>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QNetworkInterface>
#include <QTemporaryDir>
#include <QTest>

class RelayTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QProcess m_server;
    RelayConfig m_config;
    bool m_publicServer=false;
    void startServer() {
        auto env=QProcessEnvironment::systemEnvironment(); env.insert("LINKDESK_ENROLLMENT_KEY",m_config.enrollmentKey);
        m_server.setProcessEnvironment(env); m_server.setProcessChannelMode(QProcess::MergedChannels);
        m_server.start(QString::fromUtf8(LINKDESK_TEST_PYTHON), {QString::fromUtf8(LINKDESK_SERVER_SOURCE),"--bind","127.0.0.1",
            "--port",QString::number(m_config.port),"--cert",m_dir.filePath("server.pem"),"--private-key",m_dir.filePath("server.key"),"--database",m_dir.filePath("devices.sqlite3")});
        QVERIFY(m_server.waitForStarted(5000));
        QByteArray log;
        QTRY_VERIFY_WITH_TIMEOUT(([&] { log+=m_server.readAll(); return log.contains("LinkDesk relay listening on"); })(),(m_publicServer ? 30000 : 10000));
        auto match=QRegularExpression("listening on 127\\.0\\.0\\.1:([0-9]+)").match(QString::fromUtf8(log));
        QVERIFY2(match.hasMatch(),log.constData()); m_config.port=quint16(match.captured(1).toUInt());
    }
    void wireClient(RelayAgent &agent,RemoteClient &client,const QString &password) {
        connect(&agent,&RelayAgent::directReady,&client,[&client,password](const QString &host,quint16 port,const QString &id,const QString &fp){client.connectTo(host,port,password,id,fp);});
        connect(&agent,&RelayAgent::viewerReady,&client,[&client,password](quint16 port,const QString &id,const QString &fp) {
            client.connectTo("127.0.0.1",port,password,id,fp);
        });
        connect(&client,&RemoteClient::disconnected,&agent,&RelayAgent::cancelViewer);
        connect(&agent,&RelayAgent::viewerClosed,&client,&RemoteClient::disconnectPeer);
    }
private slots:
    void initTestCase() {
        Platform::initialize();
        QVERIFY(m_dir.isValid());
        QCoreApplication::setOrganizationName("LinkDeskTests"); QCoreApplication::setApplicationName("LinkDeskRelayTests");
        QStandardPaths::setTestModeEnabled(true);
        QSettings().remove("network/forceRelay");
        const QString publicConfig=qEnvironmentVariable("LINKDESK_RELAY_CONFIG");
        if (!publicConfig.isEmpty()) {
            QFile configFile(publicConfig); QVERIFY(configFile.open(QIODevice::ReadOnly));
            auto json=QJsonDocument::fromJson(configFile.readAll()).object();
            m_config.host=json.value("server").toString();
            const int port=json.value("port").toInt(443); QVERIFY(port>0 && port<=65535);
            m_config.port=quint16(port);
            m_config.fingerprint=json.value("fingerprint").toString();
            m_config.enrollmentKey=json.value("enrollmentKey").toString();
            QString error; QVERIFY2(m_config.valid(&error),qPrintable(error));
            m_publicServer=true;
            return;
        }
        if (QString::fromUtf8(LINKDESK_TEST_PYTHON).isEmpty()) QSKIP("Python 3.10+ required for local relay; public test supports LINKDESK_RELAY_CONFIG");
        QString error; QVERIFY2(Platform::generateIdentity(m_dir.filePath("server.pem"),m_dir.filePath("server.key"),&error),qPrintable(error));
        QFile file(m_dir.filePath("server.pem")); QVERIFY(file.open(QIODevice::ReadOnly));
        QSslCertificate cert(file.readAll());
        m_config.host="127.0.0.1"; m_config.port=0;
        m_config.fingerprint=QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex());
        m_config.enrollmentKey=QString(64,'a'); startServer();
    }
    void fullRelayTlsFramesAndStableId() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true,QHostAddress::LocalHost),qPrintable(error));
        RelayAgent a,b; a.setAvailable(host.port(),host.fingerprint());
        a.start(m_config,m_dir.filePath("host.json")); b.start(m_config,m_dir.filePath("viewer.json"));
        QTRY_VERIFY_WITH_TIMEOUT(a.online() && b.online(),(m_publicServer ? 30000 : 10000));
        auto hostId=a.deviceId(); QVERIFY(hostId!=b.deviceId()); QCOMPARE(hostId.size(),9);
        RemoteClient client; wireClient(b,client,host.code());
        QSignalSpy frames(&client,&RemoteClient::imageFrame),trusted(&client,&RemoteClient::certificateCheck),joined(&client,&RemoteClient::connected);
        b.connectDevice(hostId);
        QTRY_VERIFY_WITH_TIMEOUT(client.active(),(m_publicServer ? 30000 : 10000));
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(),(m_publicServer ? 30000 : 5000));
        QCOMPARE(trusted.size(),0); QCOMPARE(joined.size(),1); QVERIFY(!joined.first().at(1).toBool());
        auto image=qvariant_cast<QImage>(frames.first().first()); QVERIFY(!image.isNull()); QCOMPARE(image.size(),QSize(1280,720));
        a.stop(); QTRY_VERIFY_WITH_TIMEOUT(!client.active(),(m_publicServer ? 30000 : 5000));
        a.start(m_config,m_dir.filePath("host.json")); QTRY_VERIFY_WITH_TIMEOUT(a.online(),(m_publicServer ? 30000 : 5000)); QCOMPARE(a.deviceId(),hostId);
        b.connectDevice(hostId); QTRY_VERIFY_WITH_TIMEOUT(client.active(),(m_publicServer ? 30000 : 5000));
        host.stop(); QTRY_VERIFY_WITH_TIMEOUT(!client.active(),(m_publicServer ? 30000 : 5000));
        a.stop(); b.stop();
    }
    void sameLanUsesDirectTls() {
        RemoteHost host; QString error; QVERIFY(host.start(0,&error,true,QHostAddress::Any));
        QHostAddress local;
        for(const auto &address:QNetworkInterface::allAddresses()) if(!address.isLoopback() && (address.isInSubnet(QHostAddress("10.0.0.0"),8) || address.isInSubnet(QHostAddress("172.16.0.0"),12) || address.isInSubnet(QHostAddress("192.168.0.0"),16))) { local=address;break; }
        if(local.isNull()) QSKIP("No private IPv4 interface is available for the LAN route test.");
        QTcpServer localProbe;QVERIFY(localProbe.listen(QHostAddress::Any,0));
        QTcpSocket reachability;reachability.connectToHost(local,localProbe.serverPort());
        if(!reachability.waitForConnected(2000)) QSKIP("Local interface TCP is blocked by the host network; loopback TLS is tested separately.");
        QElapsedTimer probeTime;probeTime.start();while(!localProbe.hasPendingConnections() && probeTime.elapsed()<2000) QTest::qWait(20);
        if(!localProbe.hasPendingConnections()) QSKIP("The host network completes TCP connect without delivering LAN accepts; loopback and relay TLS remain testable.");
        reachability.abort();localProbe.close();QCoreApplication::processEvents();
        RelayAgent a,b; a.setAvailable(host.port(),host.fingerprint());
        a.start(m_config,m_dir.filePath("lan-host.json")); b.start(m_config,m_dir.filePath("lan-viewer.json"));
        QTRY_VERIFY_WITH_TIMEOUT(a.online() && b.online(),30000);
        QStringList statuses;connect(&b,&RelayAgent::status,&b,[&](const QString &message){statuses.append(message);});
        RemoteClient client; wireClient(b,client,host.code()); QSignalSpy direct(&b,&RelayAgent::directReady);
        b.connectDevice(a.deviceId()); QTRY_VERIFY_WITH_TIMEOUT(client.active(),30000); QVERIFY2(direct.size()==1,qPrintable(statuses.join('\n')));
        client.disconnectPeer(); a.stop(); b.stop();
    }
    void wrongPasswordIsRejectedInsideTunnel() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true,QHostAddress::LocalHost),qPrintable(error));
        RelayAgent a,b; a.setAvailable(host.port(),host.fingerprint());
        a.start(m_config,m_dir.filePath("wrong-host.json")); b.start(m_config,m_dir.filePath("wrong-viewer.json"));
        QTRY_VERIFY_WITH_TIMEOUT(a.online() && b.online(),(m_publicServer ? 30000 : 10000));
        RemoteClient client; wireClient(b,client,"00000000"); QSignalSpy errors(&client,&RemoteClient::error),frames(&client,&RemoteClient::imageFrame);
        b.connectDevice(a.deviceId()); QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),(m_publicServer ? 30000 : 10000));
        QVERIFY(errors.first().first().toString().contains("验证码错误")); QCOMPARE(frames.size(),0); QVERIFY(!client.active());
        a.stop(); b.stop();
    }
    void forceRelaySkipsLan() {
        QSettings().setValue("network/forceRelay",true);
        RemoteHost host; QString error; QVERIFY(host.start(0,&error,true,QHostAddress::Any));
        RelayAgent a,b; a.setAvailable(host.port(),host.fingerprint());
        a.start(m_config,m_dir.filePath("forced-host.json")); b.start(m_config,m_dir.filePath("forced-viewer.json"));
        QTRY_VERIFY_WITH_TIMEOUT(a.online() && b.online(),30000);
        RemoteClient client; wireClient(b,client,host.code()); QSignalSpy direct(&b,&RelayAgent::directReady),relayed(&b,&RelayAgent::viewerReady);
        b.connectDevice(a.deviceId()); QTRY_VERIFY_WITH_TIMEOUT(client.active(),30000); QCOMPARE(direct.size(),0); QCOMPARE(relayed.size(),1);
        client.disconnectPeer(); a.stop(); b.stop(); QSettings().remove("network/forceRelay");
    }
    void tenThumbnailsExpandAndReturnIndependently() {
        std::vector<std::unique_ptr<RemoteHost>> hosts;
        std::vector<std::unique_ptr<RelayAgent>> agents;
        RelayAgent controller;
        controller.start(m_config,m_dir.filePath("list-controller.json"));
        QTRY_VERIFY_WITH_TIMEOUT(controller.online(),30000);
        QJsonArray records; QStringList accounts;
        for(int i=0;i<10;++i) {
            auto host=std::make_unique<RemoteHost>(); QString error; QVERIFY2(host->start(0,&error,true,QHostAddress::LocalHost),qPrintable(error));
            auto agent=std::make_unique<RelayAgent>(); agent->setAvailable(host->port(),host->fingerprint()); agent->start(m_config,m_dir.filePath(QString("list-host-%1.json").arg(i)));
            QTRY_VERIFY_WITH_TIMEOUT(agent->online(),30000);
            const auto uid=QString("list-test-%1").arg(i),account="list:"+uid+":"+m_config.host.toLower()+":"+QString::number(m_config.port);
            QVERIFY(Platform::savePassword(account,host->code())); accounts.append(account);
            records.append(QJsonObject{{"uid",uid},{"target",agent->deviceId()},{"name",QString("电脑 %1").arg(i)},{"group",i<5 ? "测试分组 A" : "测试分组 B"},{"remember",true},{"automatic",true}});
            hosts.push_back(std::move(host)); agents.push_back(std::move(agent));
        }
        QSettings settings; settings.setValue("connectionList/devices",records); settings.setValue("connectionList/groups",QStringList{"默认分组","测试分组 A","测试分组 B"}); settings.setValue("network/forceRelay",true);
        {
            ConnectionList list(&controller); list.resize(1100,800); list.show(); list.reconnectAll();
            auto clients=list.findChildren<RemoteClient *>(); QCOMPARE(clients.size(),10);
            QTRY_VERIFY_WITH_TIMEOUT(std::all_of(clients.begin(),clients.end(),[](RemoteClient *c){return c->active();}),60000);
            for(auto c:clients) { QCOMPARE(c->streamSettings().width,480); QCOMPARE(c->streamSettings().fps,1); QCOMPARE(c->streamSettings().quality,20); }
            auto first=list.findChild<QWidget *>("preview-"+agents[0]->deviceId()); QVERIFY(first);
            QSignalSpy frames(clients[0],&RemoteClient::imageFrame);
            QImage synthetic(1280,720,QImage::Format_RGB32);synthetic.fill(QColor("#edf0e4"));hosts[0]->setSyntheticFrame(synthetic);
            QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(),20000);
            const auto screenshot=qEnvironmentVariable("LINKDESK_LIST_SCREENSHOT"); if(!screenshot.isEmpty()) QVERIFY(list.grab().save(screenshot));
            auto image=qvariant_cast<QImage>(frames.last().first()); QVERIFY(!image.isNull()); QCOMPARE(image.size(),QSize(480,270));
            // Idle suppression must not mark a usable thumbnail red or prevent
            // expansion after the former 15-second stale-picture threshold.
            QSignalSpy states(&list,&ConnectionList::connectionState);
            QTest::qWait(16000);
            QVERIFY(std::all_of(clients.begin(),clients.end(),[](RemoteClient *c){return c->active();}));
            for(const auto &state:states) QVERIFY2(state.at(2).toInt()!=1,qPrintable(state.at(1).toString()));
            QTest::mouseDClick(first,Qt::LeftButton); QCoreApplication::processEvents();
            auto viewers=list.findChildren<ViewerWindow *>(); QCOMPARE(viewers.size(),10);
            QTRY_COMPARE_WITH_TIMEOUT(std::count_if(clients.begin(),clients.end(),[](RemoteClient *c){return c->streamSettings().width==1920;}),1,5000);
            synthetic.fill(QColor("#70b080"));hosts[0]->setSyntheticFrame(synthetic);
            QTRY_VERIFY_WITH_TIMEOUT(qvariant_cast<QImage>(frames.last().first()).width()==1280 && qAbs(qvariant_cast<QImage>(frames.last().first()).pixelColor(0,0).green()-176)<5,10000);
            auto second=list.findChild<QWidget *>("preview-"+agents[1]->deviceId()); QVERIFY(second); QTest::mouseDClick(second,Qt::LeftButton);
            QTRY_COMPARE_WITH_TIMEOUT(std::count_if(clients.begin(),clients.end(),[](RemoteClient *c){return c->streamSettings().width==1920;}),1,5000);
            QTRY_COMPARE_WITH_TIMEOUT(std::count_if(viewers.begin(),viewers.end(),[](ViewerWindow *v){return v->isVisible();}),1,5000);
            ViewerWindow *selected=nullptr,*previous=nullptr;
            for(auto v:viewers) { if(v->windowTitle().startsWith("电脑 1")) selected=v; if(v->windowTitle().startsWith("电脑 0")) previous=v; }
            QVERIFY(selected && previous); QVERIFY(selected->isVisible()); QVERIFY(!previous->isVisible());
            // Delayed native activation from the previous window must not steal the selection.
            QEvent activate(QEvent::WindowActivate); QCoreApplication::sendEvent(previous,&activate);
            QVERIFY(selected->isVisible()); QVERIFY(!previous->isVisible()); QCOMPARE(clients[1]->streamSettings().width,1920);
            QSignalSpy expansions(selected,&ViewerWindow::expanded); QTest::mouseDClick(second,Qt::LeftButton); QCoreApplication::processEvents();
            QCOMPARE(expansions.size(),0); QVERIFY(selected->isVisible());
            for(auto v:viewers) if(v->isVisible()) v->close();
            QTRY_VERIFY_WITH_TIMEOUT(std::all_of(clients.begin(),clients.end(),[](RemoteClient *c){return c->active() && c->streamSettings().width==480;}),5000);
            list.hide(); // Remote-connect page can open a list session while the list page is hidden.
            QVERIFY(list.connectDesktop(agents[0]->deviceId(),hosts[0]->code(),false)); QCOMPARE(list.findChildren<RemoteClient *>().size(),10);
            QTRY_COMPARE_WITH_TIMEOUT(std::count_if(viewers.begin(),viewers.end(),[](ViewerWindow *v){return v->isVisible();}),1,5000);
            QCOMPARE(QSettings().value("connectionList/devices").toJsonArray().size(),10);
            list.disconnectDesktop(agents[0]->deviceId()); QTRY_COMPARE_WITH_TIMEOUT(std::count_if(clients.begin(),clients.end(),[](RemoteClient *c){return c->active();}),9,10000);
            hosts[1]->stop(); QTRY_COMPARE_WITH_TIMEOUT(std::count_if(clients.begin(),clients.end(),[](RemoteClient *c){return c->active();}),8,10000);
            list.stopAll(); QTRY_VERIFY(std::none_of(clients.begin(),clients.end(),[](RemoteClient *c){return c->active();}));
        }
        settings.remove("connectionList"); settings.remove("network/forceRelay"); for(const auto &account:accounts) Platform::removePassword(account);
        controller.stop(); for(auto &a:agents) a->stop();
    }
    void serverPinMismatchIsRejected() {
        RelayConfig wrong=m_config; wrong.fingerprint=QString(64,'0'); RelayAgent agent;
        QSignalSpy errors(&agent,&RelayAgent::error); agent.start(wrong,m_dir.filePath("wrong-pin.json"));
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),(m_publicServer ? 30000 : 10000)); QVERIFY(!agent.online()); QVERIFY(agent.deviceId().isEmpty());
        QVERIFY(errors.first().first().toString().contains("指纹不匹配")); agent.stop();
    }
    void closedSharingAndUnknownIdAreRejected() {
        RelayAgent a,b; a.start(m_config,m_dir.filePath("closed-host.json")); b.start(m_config,m_dir.filePath("closed-viewer.json"));
        QTRY_VERIFY_WITH_TIMEOUT(a.online() && b.online(),(m_publicServer ? 30000 : 10000)); QSignalSpy errors(&b,&RelayAgent::error),closed(&b,&RelayAgent::viewerClosed);
        b.connectDevice(a.deviceId()); QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),(m_publicServer ? 30000 : 5000));
        QVERIFY(errors.first().first().toString().contains("尚未开启共享")); QTRY_VERIFY(!closed.isEmpty());
        errors.clear(); b.connectDevice("000000000"); QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),(m_publicServer ? 30000 : 5000));
        QVERIFY(errors.first().first().toString().contains("离线")); a.stop(); b.stop();
    }
    void relayRestartAutomaticallyReconnects() {
        if (m_publicServer) QSKIP("Public relay is not restarted by the client test");
        RelayAgent a; a.start(m_config,m_dir.filePath("restart-host.json")); QTRY_VERIFY_WITH_TIMEOUT(a.online(),(m_publicServer ? 30000 : 10000));
        auto id=a.deviceId();
        m_server.terminate(); if (!m_server.waitForFinished(2000)) { m_server.kill(); QVERIFY(m_server.waitForFinished(5000)); } QTRY_VERIFY_WITH_TIMEOUT(!a.online(),(m_publicServer ? 30000 : 5000));
        startServer(); QTRY_VERIFY_WITH_TIMEOUT(a.online(),(m_publicServer ? 30000 : 10000)); QCOMPARE(a.deviceId(),id); a.stop();
    }
    void cleanupTestCase() { if (!m_publicServer) { m_server.terminate(); if (!m_server.waitForFinished(5000)) { m_server.kill(); m_server.waitForFinished(); } } }
};
QTEST_MAIN(RelayTest)
#include "relay_test.moc"
