#include "viewer.h"
#include "remote.h"
#include <QBuffer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSettings>
#include <QTest>
class ViewerTest : public QObject {
    Q_OBJECT
    QByteArray picture() {
        QImage image(640,360,QImage::Format_RGB32); image.fill(Qt::white);
        QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"JPEG"); return bytes;
    }
    void clickable(RemoteCanvas *canvas) {
        QSignalSpy input(canvas,&RemoteCanvas::input);
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,canvas->rect().center());
        int buttons=0; for(const auto &row:input) if(row[0].toJsonObject().value("action")=="button") ++buttons;
        QCOMPARE(buttons,2);
    }
private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("LinkDeskTests"); QCoreApplication::setApplicationName("LinkDeskViewerTests");
        QSettings().clear();
    }
    void transientHideAndFullscreenKeepInputAndFullQuality() {
        RemoteClient client; ViewerWindow viewer(&client); viewer.setManaged(true); viewer.preview(); viewer.begin("test",true);
        auto canvas=viewer.findChild<RemoteCanvas *>(); QVERIFY(canvas); canvas->setFrame(picture()); clickable(canvas);
        QSignalSpy previews(&viewer,&ViewerWindow::returnedToPreview);
        // Qt native window recreation can send Hide/Show during state changes.
        viewer.hide(); viewer.show(); QCoreApplication::processEvents();
        QCOMPARE(previews.size(),0); QCOMPARE(client.streamSettings().width,1920); clickable(canvas);
        viewer.showFullScreen(); QCoreApplication::processEvents();
        QCOMPARE(previews.size(),0); QCOMPARE(client.streamSettings().width,1920); clickable(canvas);
        viewer.showNormal(); QCoreApplication::processEvents(); clickable(canvas);
        viewer.close(); QCOMPARE(previews.size(),1); QCOMPARE(client.streamSettings().width,480);
    }
    void previewNeverAcceptsInputEvenWhenHostGrantsControl() {
        RemoteClient client; ViewerWindow viewer(&client); viewer.setManaged(true); viewer.preview();
        auto canvas=viewer.findChild<RemoteCanvas *>(); QVERIFY(canvas); canvas->setFrame(picture());
        emit client.controlChanged(true); QSignalSpy input(canvas,&RemoteCanvas::input);
        QTest::mouseClick(canvas,Qt::LeftButton,Qt::NoModifier,canvas->rect().center()); QCOMPARE(input.size(),0);
        viewer.begin("test",true); canvas->setFrame(picture()); clickable(canvas);
        viewer.showMinimized(); QCoreApplication::processEvents(); QCOMPARE(client.streamSettings().width,480);
        viewer.begin("test",true); canvas->setFrame(picture()); QVERIFY(!viewer.isMinimized()); clickable(canvas);
        emit client.controlChanged(false); input.clear(); QTest::mouseClick(canvas,Qt::LeftButton); QCOMPARE(input.size(),0);
    }
};
QTEST_MAIN(ViewerTest)
#include "viewer_test.moc"
