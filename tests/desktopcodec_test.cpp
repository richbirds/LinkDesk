#include "desktopcodec.h"
#include <QPainter>
#include <QTest>
class DesktopCodecTest : public QObject {
    Q_OBJECT
private slots:
    void nativeUnchangedReferenceHasAHeartbeat() {
        DesktopEncoder encoder;DesktopDecoder decoder;
        QVERIFY(encoder.repeatReference().isEmpty());
        QImage source(640,360,QImage::Format_RGB32);source.fill(Qt::green);
        const auto key=encoder.encode(source,90,true);encoder.commit();
        QImage image;bool changed=false;QString error;
        QVERIFY(decoder.decode(key,&image,&changed,&error));QVERIFY(changed);
        const auto heartbeat=encoder.repeatReference();QCOMPARE(heartbeat.size(),19);
        QVERIFY(decoder.decode(heartbeat,&image,&changed,&error));QVERIFY(!changed);QCOMPARE(image.size(),source.size());
    }
    void scrollCopiesAndDroppedMetadata() {
        DesktopEncoder encoder;encoder.setOfficeOptimizations(true);DesktopDecoder decoder;
        QImage source(640,360,QImage::Format_RGB32);
        for(int y=0;y<source.height();++y) for(int x=0;x<source.width();++x) source.setPixel(x,y,qRgb((x*7+y*3)%256,(y*5)%256,(x+y)%256));
        CaptureFrame first;first.image=source;first.serial=1;QImage result;bool changed;QString error;
        const auto key=encoder.encode(first,88,true,0);QVERIFY(decoder.decode(key,&result,&changed,&error));encoder.commit();
        CaptureFrame next;next.serial=2;next.previousSerial=1;next.regionsValid=true;next.image=source.copy();
        QPainter p(&next.image);p.drawImage(QRect(0,0,640,344),source,QRect(0,16,640,344));p.fillRect(0,344,640,16,Qt::white);p.end();
        next.moves.append({QPoint(0,16),QRect(0,0,640,344)});next.dirty=QRect(0,344,640,16);
        const auto packet=encoder.encode(next,88,true,33);QCOMPARE(quint8(packet[4]),quint8(5));
        auto malformed=packet;malformed[29]=char(0xff);malformed[30]=char(0xff);QVERIFY(!decoder.decode(malformed,&result,&changed,&error));
        QVERIFY(decoder.decode(packet,&result,&changed,&error));QCOMPARE(result,next.image.convertToFormat(QImage::Format_RGBA8888));
        // Drop a capture; its metadata references serial 2, not committed serial 1.
        next.previousSerial=2;next.serial=3;next.moves.clear();next.dirty=QRect(1,1,1,1);next.image.setPixelColor(500,300,Qt::red);
        const auto droppedBasePacket=encoder.encode(next,88,true,66);
        DesktopDecoder oldBase;QVERIFY(oldBase.decode(key,&result,&changed,&error));
        QVERIFY(oldBase.decode(droppedBasePacket,&result,&changed,&error));QCOMPARE(result,next.image.convertToFormat(QImage::Format_RGBA8888));
    }
    void idleRepairsLossyKeyframe() {
        DesktopEncoder encoder;encoder.setOfficeOptimizations(true);DesktopDecoder decoder;
        QImage image(640,360,QImage::Format_RGB32);quint32 random=1;
        for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x) { random=random*1664525u+1013904223u;image.setPixel(x,y,0xff000000u|(random&0xffffffu)); }
        CaptureFrame frame;frame.image=image;QImage result;bool changed;QString error;
        QVERIFY(decoder.decode(encoder.encode(frame,60,false,0),&result,&changed,&error));encoder.commit();QVERIFY(encoder.needsRefinement());
        QCOMPARE(encoder.encode(frame,60,false,300).size(),19);
        const auto refinement=encoder.encode(frame,60,false,700);QVERIFY(decoder.decode(refinement,&result,&changed,&error));QCOMPARE(result,image.convertToFormat(QImage::Format_RGBA8888));
        // A dropped repair must not clear the repair obligation.
        QVERIFY(encoder.needsRefinement());QVERIFY(encoder.encode(frame,60,false,800).size()>19);encoder.commit();QVERIFY(!encoder.needsRefinement());
    }
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
