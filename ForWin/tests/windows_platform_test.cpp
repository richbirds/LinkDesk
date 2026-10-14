#include "platform.h"
#include "input.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <QApplication>
#include <QLineEdit>
#include <QScreen>
#include <QFile>
#include <QSslCertificate>
#include <QSslKey>
#include <QTemporaryDir>
#include <QTest>
#include <QBuffer>
#include <QElapsedTimer>
class WindowsPlatformTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { Platform::initialize(); }
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
