#include "streamscheduler.h"
#include "videocodec.h"
#include <QElapsedTimer>
#include <QTest>
class StreamingTest : public QObject {
    Q_OBJECT
private slots:
    void boundsLatencyAndMatchesAcknowledgements() {
        StreamScheduler scheduler;scheduler.sent(1,0,1000,0,100);QVERIFY(!scheduler.acknowledge(9,30));
        QVERIFY(scheduler.canCapture(100,0));QVERIFY(!scheduler.canCapture(500,0));QVERIFY(scheduler.acknowledge(1,600,2));QVERIFY(scheduler.canCapture(700,0));
        scheduler.sent(2,700,1000000,699,100);QVERIFY(scheduler.acknowledge(2,701));QVERIFY(scheduler.metrics(1000).value("estimatedDeliveryBps").toDouble()>6000000);QCOMPARE(scheduler.estimatedBitrate(),qint64(0));
        QCOMPARE(scheduler.metrics(1000).value("frames").toInt(),2);scheduler.reset();QCOMPARE(scheduler.metrics(0).value("frames").toInt(),0);
    }
    void malformedVideoEnvelope() {
        QByteArray payload("\0\0\0\1abc",7),data;QSize size;quint32 sequence;QString error;
        const auto packet=videoPacket(payload,QSize(1280,720),1);QVERIFY(unpackVideoPacket(packet,&data,&size,&sequence,&error));QCOMPARE(data,payload);
        QVERIFY(!unpackVideoPacket(packet.left(packet.size()-1),&data,&size,&sequence,&error));
        auto bad=packet;bad[4]=char(0xff);bad[5]=char(0xff);QVERIFY(!unpackVideoPacket(bad,&data,&size,&sequence,&error));
    }
    void nativeHardwareRoundtripOrExplicitFallback() {
        auto encoder=createVideoEncoder();auto decoder=createVideoDecoder();QImage image(640,360,QImage::Format_RGB32);image.fill(QColor(90,150,180));
        QString error;const auto bytes=encoder->encode(image,{image.size(),30,2000000},&error);
        if(bytes.isEmpty()) { QVERIFY(!error.isEmpty());QSKIP(qPrintable(error)); }
        QVERIFY(encoder->hardware());const auto result=decoder->decode(bytes,image.size(),&error);QVERIFY2(!result.isNull(),qPrintable(error));QCOMPARE(result.size(),image.size());
        const auto pixel=result.pixelColor(100,100);QVERIFY(qAbs(pixel.red()-90)<12);QVERIFY(qAbs(pixel.green()-150)<12);QVERIFY(qAbs(pixel.blue()-180)<12);
        QElapsedTimer timer;timer.start();
        for(int n=0;n<5;++n) { image.fill(QColor(90+n*3,150,180));const auto packet=encoder->encode(image,{image.size(),30,2000000},&error);QVERIFY2(!packet.isEmpty(),qPrintable(error));const auto next=decoder->decode(packet,image.size(),&error);QVERIFY2(!next.isNull(),qPrintable(error)); }
        qInfo("Hardware H.264 steady roundtrip mean: %.2f ms",timer.nsecsElapsed()/5000000.0);
        encoder->reset();decoder->reset();
    }
};
QTEST_MAIN(StreamingTest)
#include "streaming_test.moc"
