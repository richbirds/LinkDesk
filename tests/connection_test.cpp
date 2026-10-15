#include "remote.h"
#include "platform.h"
#include <QImage>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QFile>
#include <QCryptographicHash>
#include <QUuid>
#include <QTemporaryDir>
#include <QSettings>
#include <QDir>
class ConnectionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        Platform::initialize();
        QCoreApplication::setOrganizationName("LinkDeskTests");
        QCoreApplication::setApplicationName("LinkDeskTests");
        QStandardPaths::setTestModeEnabled(true);
    }
    void fileTransferResumesWithoutOverwriting() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());
        const auto receive=temporary.path()+"/receive";QVERIFY(QDir().mkpath(receive));
        QSettings().setValue("files/receiveDirectory",receive);
        const QString source=temporary.path()+"/example.dat";QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray payload(4*1024*1024,'x');for(int n=0;n<payload.size();++n)payload[n]=char(n*31+n/731);
        QCOMPARE(file.write(payload),qint64(payload.size()));file.close();
        QFile existing(receive+"/example.dat");QVERIFY(existing.open(QIODevice::WriteOnly));existing.write("preserve");existing.close();
        RemoteHost host;QString error;QVERIFY2(host.start(0,&error,true),qPrintable(error));
        RemoteClient client;QSignalSpy complete(&host,&RemoteHost::fileReceived),frames(&client,&RemoteClient::imageFrame);
        client.connectTo("127.0.0.1",host.port(),host.code(),"transfer-test",host.fingerprint());
        QTRY_VERIFY_WITH_TIMEOUT(client.active(),10000);
        qint64 acknowledged=0;bool interrupted=false;
        auto watch=connect(&client,&RemoteClient::fileProgress,&client,[&](const QString &,qint64 done,qint64,const QString &state){
            if(state=="传送中") acknowledged=done;
        });
        client.sendFiles({source});QTRY_VERIFY_WITH_TIMEOUT(acknowledged>=256*1024,10000);
        client.disconnectPeer();QTRY_VERIFY(!client.active());QTest::qWait(150);disconnect(watch);interrupted=true;
        QVERIFY(interrupted);QCOMPARE(complete.size(),0);
        const auto partials=QDir(receive+"/.linkdesk-partials").entryList({"*.part"},QDir::Files);QCOMPARE(partials.size(),1);
        const qint64 saved=QFileInfo(receive+"/.linkdesk-partials/"+partials.first()).size();QVERIFY(saved>0);QVERIFY(saved<payload.size());
        bool resumed=false;connect(&client,&RemoteClient::fileProgress,&client,[&](const QString &,qint64 done,qint64,const QString &state){if(state=="传送中" && done>=saved)resumed=true;});
        client.connectTo("127.0.0.1",host.port(),host.code(),"transfer-test",host.fingerprint());
        QTRY_VERIFY_WITH_TIMEOUT(client.active(),10000);QTRY_COMPARE_WITH_TIMEOUT(complete.size(),1,20000);QVERIFY(resumed);QVERIFY(!frames.isEmpty());
        QFile received(complete.first().first().toString());QVERIFY(received.open(QIODevice::ReadOnly));QCOMPARE(received.readAll(),payload);
        QVERIFY(existing.open(QIODevice::ReadOnly));QCOMPARE(existing.readAll(),QByteArray("preserve"));
        // Empty files and multiple queued files use the same authenticated stream.
        QFile empty(temporary.path()+"/empty.txt");QVERIFY(empty.open(QIODevice::WriteOnly));empty.close();
        client.sendFiles({empty.fileName(),source});QTRY_COMPARE_WITH_TIMEOUT(complete.size(),3,20000);QVERIFY(client.active());
        client.disconnectPeer();host.stop();QSettings().remove("files/receiveDirectory");
    }
    void corruptedResumeIsNeverPublished() {
        QTemporaryDir temporary;const auto receive=temporary.path()+"/receive";QVERIFY(QDir().mkpath(receive+"/.linkdesk-partials"));
        QSettings().setValue("files/receiveDirectory",receive);
        const QByteArray payload(512*1024,'c');const QString name="corrupt.dat",source=temporary.path()+"/"+name;
        QFile file(source);QVERIFY(file.open(QIODevice::WriteOnly));file.write(payload);file.close();
        const auto hash=QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex();
        const auto id=QCryptographicHash::hash(hash+name.toUtf8(),QCryptographicHash::Sha256).toHex();
        QFile partial(receive+"/.linkdesk-partials/"+id+".part");QVERIFY(partial.open(QIODevice::WriteOnly));partial.write(QByteArray(65536,'z'));partial.close();
        RemoteHost host;QString error;QVERIFY(host.start(0,&error,true));RemoteClient client;
        QSignalSpy complete(&host,&RemoteHost::fileReceived),progress(&client,&RemoteClient::fileProgress),power(&client,&RemoteClient::powerResult);
        client.connectTo("127.0.0.1",host.port(),host.code(),"test",host.fingerprint());QTRY_VERIFY_WITH_TIMEOUT(client.active(),10000);
        client.sendFiles({source});bool failed=false;
        QTRY_VERIFY_WITH_TIMEOUT(([&]{for(const auto &entry:progress)if(entry[3].toString().contains("完整性校验失败"))failed=true;return failed;})(),10000);
        QCOMPARE(complete.size(),0);QVERIFY(!QFile::exists(receive+"/"+name));QVERIFY(!partial.exists());
        client.requestPower("shutdown");QTRY_COMPARE_WITH_TIMEOUT(power.size(),1,5000);QVERIFY(power.first().first().toString().contains("未授权"));
        client.disconnectPeer();host.stop();QSettings().remove("files/receiveDirectory");
    }
    void customPasswordAndSecureStorage() {
        RemoteHost host; QString error; const auto old=host.code();
        QVERIFY(!host.setCode("short",&error)); QCOMPARE(host.code(),old);
        const QString code="MiXeD-Password-42"; QVERIFY(host.setCode(code,&error)); QCOMPARE(host.code(),code);
        QVERIFY(host.start(0,&error,true));
        RemoteClient wrong; QSignalSpy rejected(&wrong,&RemoteClient::error);
        wrong.connectTo("127.0.0.1",host.port(),code.toLower(),"test",host.fingerprint());
        QTRY_VERIFY_WITH_TIMEOUT(!rejected.isEmpty(),10000); QVERIFY(!wrong.active());
        RemoteClient client; client.connectTo("127.0.0.1",host.port(),code,"test",host.fingerprint());
        QTRY_VERIFY_WITH_TIMEOUT(client.active(),10000);
        const auto account="test-password-"+QUuid::createUuid().toString();
        QVERIFY(Platform::savePassword(account,code)); QCOMPARE(Platform::loadPassword(account),code);
        QVERIFY(Platform::savePassword(account,"Updated-password-43")); QCOMPARE(Platform::loadPassword(account),QString("Updated-password-43"));
        QVERIFY(Platform::removePassword(account)); QVERIFY(Platform::loadPassword(account).isEmpty());
    }
    void localPasswordStoreRejectsTampering() {
#ifdef Q_OS_MAC
        const QString account="vault-test-"+QUuid::createUuid().toString(); const QString secret="local-only-secret";
        QVERIFY(Platform::savePassword(account,secret)); QCOMPARE(Platform::loadPassword(account),secret);
        const auto path=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/passwords-local/"+QString::fromLatin1(QCryptographicHash::hash(account.toUtf8(),QCryptographicHash::Sha256).toHex())+".bin";
        QFile file(path); QVERIFY(file.open(QIODevice::ReadWrite)); auto bytes=file.readAll(); QVERIFY(!bytes.contains(secret.toUtf8()));
        bytes[20]=char(bytes[20]^1); QVERIFY(file.seek(0)); QCOMPARE(file.write(bytes),qint64(bytes.size())); file.close();
        QVERIFY(Platform::loadPassword(account).isEmpty()); QVERIFY(Platform::removePassword(account));
#endif
    }
    void authenticatedFramesAndRotation() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        RemoteClient client; client.setProperty("legacyFrames",true); QSignalSpy legacy(&client,&RemoteClient::frame);
        connect(&client,&RemoteClient::certificateCheck,&client,[&](const QString &,const QString &hash) {
            QCOMPARE(hash,host.fingerprint()); client.trustCertificate(true);
        });
        QSignalSpy connected(&client,&RemoteClient::connected),frames(&client,&RemoteClient::imageFrame),left(&client,&RemoteClient::disconnected);
        client.connectTo("127.0.0.1",host.port(),host.code());
        QTRY_COMPARE_WITH_TIMEOUT(connected.size(),1,10000);
        QVERIFY(!connected.first().at(1).toBool()); // Viewing is the default.
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(),5000);
        QVERIFY(!legacy.isEmpty());
        auto image=qvariant_cast<QImage>(frames.first().first()); QVERIFY(!image.isNull());
        QCOMPARE(image.size(),QSize(1280,720));
        host.rotateCode(); QTRY_VERIFY_WITH_TIMEOUT(!left.isEmpty(),5000); QVERIFY(!client.active());
        host.stop(); QVERIFY(!host.isListening());
    }
    void negotiatedLosslessFramesAndLimits() {
        const auto bounded=StreamSettings::fromJson({{"fps",999},{"quality",0},{"mbps",999},{"width",9999},{"lossless",true}});
        QCOMPARE(bounded.fps,30); QCOMPARE(bounded.quality,15); QCOMPARE(bounded.width,1920);
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        RemoteClient client; StreamSettings settings; settings.lossless=true; settings.fps=1; settings.width=1280;
        client.setStreamSettings(settings);
        QSignalSpy applied(&client,&RemoteClient::streamSettingsApplied),frames(&client,&RemoteClient::imageFrame);
        client.connectTo("127.0.0.1",host.port(),host.code(),"test",host.fingerprint());
        QTRY_VERIFY_WITH_TIMEOUT(!applied.isEmpty(),10000);
        QVERIFY(applied.first().first().toJsonObject().value("lossless").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(),5000);
        const auto staticCount=frames.size();QTest::qWait(1200);QCOMPARE(frames.size(),staticCount);QVERIFY(client.active());
        bool png=false;
        for (const auto &args : frames) {
            auto image=qvariant_cast<QImage>(args.first()); QVERIFY(!image.isNull()); QCOMPARE(image.size(),QSize(1280,720));
            QCOMPARE(image.pixelColor(0,0),QColor("#101b32")); png=true;
        }
        QVERIFY(png);
        settings.lossless=false; settings.fps=15; settings.quality=95; client.setStreamSettings(settings);
        QTRY_VERIFY_WITH_TIMEOUT(applied.size()>=2,5000);
        QCOMPARE(applied.last().first().toJsonObject().value("fps").toInt(),15);
    }
    void wrongCodeNeverGetsFrames() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        RemoteClient client;
        connect(&client,&RemoteClient::certificateCheck,&client,[&](const QString &,const QString &) { client.trustCertificate(true); });
        QSignalSpy errors(&client,&RemoteClient::error),connected(&client,&RemoteClient::connected),frames(&client,&RemoteClient::imageFrame);
        client.connectTo("127.0.0.1",host.port(),"000000");
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),10000);
        QVERIFY(errors.first().first().toString().contains("验证码错误"));
        QCOMPARE(connected.size(),0); QCOMPARE(frames.size(),0); QVERIFY(!client.active());
    }
    void videoSwitchAndUnavailableEncoderFallback() {
        const auto saved=qgetenv("LINKDESK_DISABLE_H264");
        for(bool disabled:{false,true}) {
            if(disabled) qputenv("LINKDESK_DISABLE_H264","1");else qunsetenv("LINKDESK_DISABLE_H264");
            RemoteHost host;QString error;QVERIFY2(host.start(0,&error,true),qPrintable(error));
            QImage source(1280,720,QImage::Format_RGB32);source.fill(QColor(90,150,180));host.setSyntheticFrame(source);
            RemoteClient client;StreamSettings settings;settings.width=1280;settings.height=720;settings.codec="h264";client.setStreamSettings(settings);
            QImage received;connect(&client,&RemoteClient::imageFrame,&client,[&](const QImage &image,qint64,bool){received=image;});
            client.connectTo("127.0.0.1",host.port(),host.code(),"synthetic",host.fingerprint());
            QTRY_VERIFY_WITH_TIMEOUT(!received.isNull(),10000);
            QTRY_VERIFY_WITH_TIMEOUT(host.streamingMetrics().value("videoFrames").toInt()>0 || host.streamingMetrics().value("videoFallbacks").toInt()>0,5000);
            if(disabled) { QCOMPARE(host.streamingMetrics().value("videoFrames").toInt(),0);QVERIFY(host.streamingMetrics().value("videoFallbacks").toInt()>0); }
            // Idle repair must restore exact pixels after video or a lossy image fallback.
            QTRY_VERIFY_WITH_TIMEOUT(received.convertToFormat(QImage::Format_RGB32)==source,5000);
            source.fill(QColor(200,80,40));settings.lossless=true;client.setStreamSettings(settings);host.setSyntheticFrame(source);
            QTRY_VERIFY_WITH_TIMEOUT(received.convertToFormat(QImage::Format_RGB32)==source,5000);QVERIFY(client.active());
            client.disconnectPeer();host.stop();
        }
        if(saved.isNull()) qunsetenv("LINKDESK_DISABLE_H264");else qputenv("LINKDESK_DISABLE_H264",saved);
    }
    void rejectingCertificateNeverAuthenticates() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        RemoteClient client; QSignalSpy peers(&host,&RemoteHost::peerChanged),left(&client,&RemoteClient::disconnected),certs(&client,&RemoteClient::certificateCheck);
        connect(&client,&RemoteClient::certificateCheck,&client,[&](const QString &,const QString &) { client.trustCertificate(false); });
        client.connectTo("127.0.0.1",host.port(),host.code());
        QTRY_VERIFY_WITH_TIMEOUT(!left.isEmpty(),10000); QCOMPARE(certs.size(),1); QCOMPARE(peers.size(),0); QVERIFY(!client.active());
    }
    void concurrentClientIsRejected() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        RemoteClient a,b;
        for (auto c : {&a,&b}) connect(c,&RemoteClient::certificateCheck,c,[c](const QString &,const QString &) { c->trustCertificate(true); });
        QSignalSpy joined(&a,&RemoteClient::connected),rejected(&b,&RemoteClient::error);
        a.connectTo("127.0.0.1",host.port(),host.code()); QTRY_COMPARE_WITH_TIMEOUT(joined.size(),1,10000);
        b.connectTo("127.0.0.1",host.port(),host.code()); QTRY_VERIFY_WITH_TIMEOUT(!rejected.isEmpty(),10000);
        QVERIFY(rejected.first().first().toString().contains("已有远程会话")); QVERIFY(a.active()); QVERIFY(!b.active());
        host.stop(); QTRY_VERIFY(!a.active());
    }
    void connectionFailureIsReported() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        auto port=host.port(); host.stop();
        RemoteClient client; QSignalSpy errors(&client,&RemoteClient::error),left(&client,&RemoteClient::disconnected);
        client.connectTo("127.0.0.1",port,"123456");
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),5000); QTRY_VERIFY(!left.isEmpty()); QVERIFY(!client.active());
    }
    void failedAttemptsAreThrottled() {
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        for (int attempt=0; attempt<5; ++attempt) {
            RemoteClient client;
            connect(&client,&RemoteClient::certificateCheck,&client,[&](const QString &,const QString &) { client.trustCertificate(true); });
            QSignalSpy errors(&client,&RemoteClient::error),left(&client,&RemoteClient::disconnected);
            client.connectTo("127.0.0.1",host.port(),"000000");
            QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),5000); QTRY_VERIFY(!left.isEmpty());
            QVERIFY(errors.first().first().toString().contains("验证码错误"));
        }
        RemoteClient blocked; QSignalSpy certs(&blocked,&RemoteClient::certificateCheck),left(&blocked,&RemoteClient::disconnected);
        blocked.connectTo("127.0.0.1",host.port(),host.code());
        QTRY_VERIFY_WITH_TIMEOUT(!left.isEmpty(),5000); QCOMPARE(certs.size(),0); QVERIFY(!blocked.active());
    }
};
QTEST_MAIN(ConnectionTest)
#include "connection_test.moc"
