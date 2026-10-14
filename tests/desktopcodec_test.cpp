#include "desktopcodec.h"
#include <QPainter>
#include <QTest>
class DesktopCodecTest : public QObject {
    Q_OBJECT
private slots:
    void staticAndTypingRemainPixelExact() {
        DesktopEncoder encoder; DesktopDecoder decoder; QImage source(1920,1080,QImage::Format_RGB32); source.fill(Qt::white);
        QPainter paint(&source); paint.setPen(Qt::black); paint.drawText(QPoint(200,200),"LinkDesk clear text"); paint.end();
        QString error; bool changed=false; QImage decoded;
        const auto full=encoder.encode(source,88,true); QVERIFY(decoder.decode(full,&decoded,&changed,&error)); QVERIFY(changed); QCOMPARE(decoded,source.convertToFormat(QImage::Format_RGBA8888)); encoder.commit();
        auto idle=encoder.encode(source,88,true); QCOMPARE(idle.size(),19); QVERIFY(decoder.decode(idle,&decoded,&changed,&error)); QVERIFY(!changed);
        QPainter edit(&source); edit.fillRect(200,220,40,20,Qt::blue); edit.end();
        const auto delta=encoder.encode(source,88,true); QVERIFY(delta.size()<full.size()); QVERIFY(decoder.decode(delta,&decoded,&changed,&error)); QVERIFY(changed); QCOMPARE(decoded,source.convertToFormat(QImage::Format_RGBA8888)); encoder.commit();
        qInfo("Desktop codec: full=%lld bytes; typing=%lld; static=%lld",qint64(full.size()),qint64(delta.size()),qint64(idle.size()));
    }
    void unsentFrameNeverBecomesReference() {
        DesktopEncoder encoder; DesktopDecoder decoder; QImage source(640,360,QImage::Format_RGB32); source.fill(Qt::white);
        QImage result; bool changed; QString error; QVERIFY(decoder.decode(encoder.encode(source,88,true),&result,&changed,&error)); encoder.commit();
        source.setPixelColor(10,10,Qt::red); encoder.encode(source,88,true); // Dropped, no commit.
        source.setPixelColor(300,100,Qt::blue); auto next=encoder.encode(source,88,true);
        QVERIFY(decoder.decode(next,&result,&changed,&error)); QCOMPARE(result,source.convertToFormat(QImage::Format_RGBA8888)); encoder.commit();
        encoder.reset(); source=source.scaled(320,180); QVERIFY(decoder.decode(encoder.encode(source,88,true),&result,&changed,&error)); QCOMPARE(result.size(),source.size());
    }
    void malformedAndMissingBaseRejected() {
        DesktopEncoder encoder; QImage source(640,360,QImage::Format_RGB32); source.fill(Qt::white);
        auto key=encoder.encode(source,88,true); encoder.commit(); source.setPixelColor(10,10,Qt::red); auto patch=encoder.encode(source,88,true);
        DesktopDecoder decoder; QImage result; bool changed; QString error;
        QVERIFY(!decoder.decode(patch,&result,&changed,&error)); QVERIFY(decoder.decode(key,&result,&changed,&error));
        QVERIFY(!decoder.decode(patch.left(patch.size()-1),&result,&changed,&error));
        auto invalid=patch; invalid[23]=char(0xff); invalid[24]=char(0xff); QVERIFY(!decoder.decode(invalid,&result,&changed,&error));
        QVERIFY(decoder.decode(patch,&result,&changed,&error)); QVERIFY(!decoder.decode(patch,&result,&changed,&error));
    }
};
QTEST_MAIN(DesktopCodecTest)
#include "desktopcodec_test.moc"
