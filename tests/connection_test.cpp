#include "remote.h"
#include "platform.h"
#include <QImage>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QFile>
#include <QCryptographicHash>
#include <QUuid>
class ConnectionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        Platform::initialize();
        QCoreApplication::setOrganizationName("LinkDeskTests");
        QCoreApplication::setApplicationName("LinkDeskTests");
        QStandardPaths::setTestModeEnabled(true);
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
        QTRY_VERIFY_WITH_TIMEOUT(frames.size()>=2,5000);
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
