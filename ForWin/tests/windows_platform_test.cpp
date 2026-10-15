#include "platform.h"
#include "input.h"
#include "remote.h"
#include <QSignalSpy>
#include <QStandardPaths>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <QApplication>
#include <QLineEdit>
#include <QMainWindow>
#include <QScreen>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QTemporaryDir>
#include <QTest>
#include <QBuffer>
#include <QElapsedTimer>
#include <QTimer>
class WindowsPlatformTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setStyle("Fusion"); Platform::initialize(); }
    void nativeCertificateWithoutOpenSsl() {
        QTemporaryDir dir; QVERIFY(dir.isValid()); QString error;
        QVERIFY2(Platform::generateIdentity(dir.filePath("cert.pem"),dir.filePath("key.pem"),&error),qPrintable(error));
        QFile cert(dir.filePath("cert.pem")),key(dir.filePath("key.pem"));
        QVERIFY(cert.open(QIODevice::ReadOnly)); QVERIFY(key.open(QIODevice::ReadOnly));
        QVERIFY(!QSslCertificate(cert.readAll()).isNull());
        QVERIFY(!QSslKey(key.readAll(),QSsl::Rsa,QSsl::Pem).isNull());
    }
    void realDesktopCapture() {
        auto screen=QGuiApplication::primaryScreen(); QVERIFY(screen);
        auto rect=Platform::screenGeometry(screen); QVERIFY(!rect.isEmpty());
        auto image=Platform::captureScreen(screen); QVERIFY(!image.isNull()); QCOMPARE(image.size(),rect.size().scaled(rect.size().boundedTo(QSize(1920,1080)),Qt::KeepAspectRatio));
        qInfo("Captured Windows desktop: %d x %d",image.width(),image.height());
    }
    void captureAndEncodeTiming() {
        auto screen=QGuiApplication::primaryScreen(); QElapsedTimer timer; qint64 capture=0,encode=0,bytes=0;
        for(int i=0;i<5;++i) {
            timer.start(); auto image=Platform::captureScreen(screen); capture+=timer.elapsed(); QVERIFY(!image.isNull());
            timer.restart(); QByteArray jpeg; QBuffer buffer(&jpeg); buffer.open(QIODevice::WriteOnly); QVERIFY(image.save(&buffer,"JPEG",65)); encode+=timer.elapsed(); bytes+=jpeg.size();
        }
        qInfo("Desktop timing: capture=%lld ms encode=%lld ms JPEG=%lld bytes per frame",capture/5,encode/5,bytes/5);
    }
    void remoteCaptionButtonsKeepStreaming() {
        QCoreApplication::setOrganizationName("LinkDeskTests");
        QCoreApplication::setApplicationName("LinkDeskCaptionTests");
        QStandardPaths::setTestModeEnabled(true);
        QMainWindow window; window.setWindowTitle("LinkDesk caption regression");
        window.resize(800,500); window.show(); window.raise(); window.activateWindow();
        QTRY_VERIFY(window.isActiveWindow());
        RemoteHost host; QString error; QVERIFY2(host.start(0,&error,true),qPrintable(error));
        QVERIFY(host.setControl(true,&error));
        QImage source(640,360,QImage::Format_RGB32); source.fill(Qt::red); host.setSyntheticFrame(source);
        RemoteClient client; StreamSettings settings; settings.lossless=true; settings.width=640; settings.height=360;
        client.setStreamSettings(settings);
        QImage received; connect(&client,&RemoteClient::imageFrame,&client,[&](const QImage &image,qint64,bool){received=image;});
        QSignalSpy metrics(&host,&RemoteHost::streamMetrics);
        client.connectTo("127.0.0.1",host.port(),host.code(),"caption-test",host.fingerprint());
        QTRY_VERIFY_WITH_TIMEOUT(client.active() && !received.isNull(),10000);
        const auto geometry=Platform::screenGeometry(window.screen());
        auto buttonCenter=[&](int hit) {
            const HWND hwnd=reinterpret_cast<HWND>(window.winId());
            TITLEBARINFOEX info{}; info.cbSize=sizeof(info);
            SendMessageW(hwnd,WM_GETTITLEBARINFOEX,0,reinterpret_cast<LPARAM>(&info));
            const auto r=info.rgrect[hit==HTMINBUTTON ? 2 : hit==HTMAXBUTTON ? 3 : 5];
            return QPoint((r.left+r.right)/2,(r.top+r.bottom)/2);
        };
        auto clickCaption=[&](int hit) {
            const auto target=buttonCenter(hit);
            QVERIFY2(!target.isNull(),"Native caption button not found");
            QJsonObject event{{"action","button"},{"button",1},{"x",double(target.x()-geometry.x())/(geometry.width()-1)},
                {"y",double(target.y()-geometry.y())/(geometry.height()-1)},{"down",true}};
            client.sendInput(event);
            // The release arrives in a later socket event, as over the Internet.
            QTest::qWait(250); event["down"]=false; client.sendInput(event);
        };
        for(int cycle=0;cycle<3;++cycle) {
            clickCaption(HTMAXBUTTON); QTRY_VERIFY_WITH_TIMEOUT(window.isMaximized(),3000);
            clickCaption(HTMINBUTTON); QTRY_VERIFY_WITH_TIMEOUT(window.isMinimized(),3000);
            const auto count=metrics.size();
            source.fill(cycle%2 ? Qt::green : Qt::blue); host.setSyntheticFrame(source);
            QTRY_VERIFY_WITH_TIMEOUT(received.pixelColor(0,0)==source.pixelColor(0,0),5000);
            QTRY_VERIFY_WITH_TIMEOUT(metrics.size()>count,3000); QVERIFY(client.active());
            window.showNormal(); window.raise(); window.activateWindow();
            QTRY_VERIFY(window.isActiveWindow());
        }
        // Also cover native/VM input that bypasses InputController entirely.
        const HWND hwnd=reinterpret_cast<HWND>(window.winId());
        const auto target=buttonCenter(HTMINBUTTON);
        QVERIFY(!target.isNull());
        PostMessageW(hwnd,WM_NCLBUTTONDOWN,HTMINBUTTON,MAKELPARAM(target.x(),target.y()));
        QTimer::singleShot(250,&window,[=] {
            POINT point{target.x(),target.y()}; ScreenToClient(hwnd,&point);
            PostMessageW(hwnd,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y));
        });
        QTRY_VERIFY_WITH_TIMEOUT(window.isMinimized(),3000);
        const auto count=metrics.size(); source.fill(Qt::yellow); host.setSyntheticFrame(source);
        QTRY_VERIFY_WITH_TIMEOUT(received.pixelColor(0,0)==QColor(Qt::yellow),5000);
        QTRY_VERIFY_WITH_TIMEOUT(metrics.size()>count,3000); QVERIFY(client.active());
        client.disconnectPeer(); host.stop();
    }
    void nativeMouseAndKeyboardInTestWindow() {
        struct CursorRestore { POINT old{}; CursorRestore() { GetCursorPos(&old); } ~CursorRestore() { SetCursorPos(old.x,old.y); } } restore;
        struct LayoutRestore { HKL old=GetKeyboardLayout(0); ~LayoutRestore() { ActivateKeyboardLayout(old,0); } } layout;
        HKL english=LoadKeyboardLayoutW(L"00000409",KLF_NOTELLSHELL); QVERIFY(english);
        QLineEdit edit; edit.setInputMethodHints(Qt::ImhLatinOnly); edit.setAttribute(Qt::WA_InputMethodEnabled,false); edit.setWindowTitle("LinkDesk input verification"); edit.resize(420,100); edit.show(); edit.raise(); edit.activateWindow();
        QTRY_VERIFY_WITH_TIMEOUT(edit.isActiveWindow(),5000);
        InputController input; QString reason; QVERIFY2(input.available(&reason),qPrintable(reason));
        auto screen=edit.screen(); const auto rect=Platform::screenGeometry(screen);
        const auto logical=edit.mapToGlobal(edit.rect().center());
        const auto qtRect=screen->geometry(); const qreal scale=qreal(rect.width())/qtRect.width();
        const QPoint p(rect.x()+qRound((logical.x()-qtRect.x())*scale),rect.y()+qRound((logical.y()-qtRect.y())*scale));
        const double x=double(p.x()-rect.x())/(rect.width()-1),y=double(p.y()-rect.y())/(rect.height()-1);
        input.apply({{"action","button"},{"x",x},{"y",y},{"button",1},{"down",true}},rect);
        input.apply({{"action","button"},{"x",x},{"y",y},{"button",1},{"down",false}},rect);
        QTest::qWait(200);
        qInfo("After mouse click: foreground=%llu expected=%llu focus=%d",quint64(quintptr(GetForegroundWindow())),quint64(edit.winId()),edit.hasFocus());
        QTRY_VERIFY(edit.isActiveWindow() && edit.hasFocus());
        // Windows can restore an IME layout when a window gains focus. Switch
        // this test window to English after the native mouse activation.
        QVERIFY(ActivateKeyboardLayout(english,0));
        SendMessageW(reinterpret_cast<HWND>(edit.winId()),WM_INPUTLANGCHANGEREQUEST,0,reinterpret_cast<LPARAM>(english));
        QTest::qWait(200);
        qInfo("Test keyboard layout: %llx",quint64(quintptr(GetKeyboardLayout(0))));
        for (int key : {int(Qt::Key_A),int(Qt::Key_B)}) {
            input.apply({{"action","key"},{"key",key},{"down",true}},rect);
            input.apply({{"action","key"},{"key",key},{"down",false}},rect);
        }
        QTRY_COMPARE(edit.text().toLower(),QString("ab"));
        input.apply({{"action","key"},{"key",int(Qt::Key_Control)},{"down",true}},rect);
        QTRY_VERIFY(GetAsyncKeyState(VK_CONTROL)&0x8000);
        input.releaseAll(); QTRY_VERIFY(!(GetAsyncKeyState(VK_CONTROL)&0x8000));
    }
};
QTEST_MAIN(WindowsPlatformTest)
#include "windows_platform_test.moc"
