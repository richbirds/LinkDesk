#pragma once
#include <QQueue>
#include <QJsonObject>
// Bounds latency and memory, not bytes per second. Delivery estimates include decoding.
class StreamScheduler {
public:
    struct Pending { quint64 id; qint64 sent,bytes,captured,encodeUs; };
    void reset();
    bool canCapture(qint64 now,qint64 socketBytes) const;
    void sent(quint64 id,qint64 now,qint64 bytes,qint64 captured,qint64 encodeUs);
    bool acknowledge(quint64 id,qint64 now,double decodeMs=0);
    qint64 queueBudget() const;
    qint64 estimatedBitrate() const;
    QJsonObject metrics(qint64 now) const;
private:
    QQueue<Pending> m_pending;
    qint64 m_bytes=0,m_frames=0,m_start=-1,m_lastAck=-1;
    double m_ackMs=0,m_decodeMs=0,m_queueMs=0,m_encodeMs=0,m_deliveryBytesPerSecond=0;
    double m_minAckMs=0;
    qint64 m_encodeTotalUs=0,m_queueTotalMs=0,m_ackTotalMs=0,m_ackedFrames=0;
};
