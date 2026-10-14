#include "protocol.h"
#include <QSignalSpy>
#include <QTest>
#include <QtEndian>
class ProtocolTest : public QObject {
    Q_OBJECT
private slots:
    void fragmented() {
        Wire wire(nullptr); QSignalSpy messages(&wire,&Wire::jsonReceived);
        const auto packet=Wire::packet(Wire::Json,"{\"type\":\"ping\"}");
        for (char b : packet) wire.feed(QByteArray(1,b));
        QCOMPARE(messages.size(),1);
        QCOMPARE(messages.first().first().value<QJsonObject>().value("type").toString(),QString("ping"));
    }
    void coalesced() {
        Wire wire(nullptr); QSignalSpy messages(&wire,&Wire::jsonReceived),frames(&wire,&Wire::frameReceived),desktop(&wire,&Wire::desktopReceived);
        wire.feed(Wire::packet(Wire::Json,"{}") + Wire::packet(Wire::Frame,"test-jpeg") + Wire::packet(Wire::Desktop,"test-delta") + Wire::packet(Wire::Json,"{}"));
        QCOMPARE(messages.size(),2); QCOMPARE(frames.size(),1); QCOMPARE(desktop.size(),1);
    }
    void oversized_data() {
        QTest::addColumn<int>("type"); QTest::addColumn<quint32>("length");
        QTest::newRow("json") << 1 << quint32(Wire::MaxJson+1);
        QTest::newRow("desktop") << 3 << quint32(Wire::MaxFrame+1);
        QTest::newRow("image") << 2 << quint32(Wire::MaxFrame+1);
        QTest::newRow("unknown-type") << 9 << quint32(1);
        QTest::newRow("zero-length") << 1 << quint32(0);
    }
    void oversized() {
        QFETCH(int,type); QFETCH(quint32,length);
        Wire wire(nullptr); QSignalSpy errors(&wire,&Wire::invalidPacket),messages(&wire,&Wire::jsonReceived);
        QByteArray header(5,'\0'); header[0]=char(type); qToBigEndian(length,header.data()+1);
        wire.feed(header); QCOMPARE(errors.size(),1);
        wire.feed(Wire::packet(Wire::Json,"{}")); QCOMPARE(messages.size(),0);
    }
    void malformedJson() {
        Wire wire(nullptr); QSignalSpy errors(&wire,&Wire::invalidPacket);
        wire.feed(Wire::packet(Wire::Json,"[]")); QCOMPARE(errors.size(),1);
    }
};
QTEST_GUILESS_MAIN(ProtocolTest)
#include "protocol_test.moc"
