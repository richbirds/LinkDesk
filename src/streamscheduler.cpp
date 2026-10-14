#include "streamscheduler.h"
#include <QtGlobal>
void StreamScheduler::reset() { *this=StreamScheduler(); }
qint64 StreamScheduler::queueBudget() const { return qBound(qint64(256*1024),qint64(m_deliveryBytesPerSecond*.12),qint64(8*1024*1024)); }
qint64 StreamScheduler::estimatedBitrate() const {
    // Application-limited typing traffic is not a measurement of link capacity.
    // Only use delivery throughput to adapt video when acknowledgement delay grows.
    const bool congested=m_ackMs>qMax(100.0,m_minAckMs*2+60) && m_pending.size()>2;
    return congested && m_deliveryBytesPerSecond>0 ? qMax<qint64>(128000,qint64(m_deliveryBytesPerSecond*8*.85)) : 0;
}
bool StreamScheduler::canCapture(qint64 now,qint64 queued) const {
    if(queued>queueBudget() || m_pending.size()>=8) return false;
    // Stop adding stale frames when the oldest outstanding frame exceeds the
    // measured round trip plus one small presentation window.
    return m_pending.isEmpty() || now-m_pending.head().sent<qMax(250.0,m_ackMs*1.5+120.0);
}
void StreamScheduler::sent(quint64 id,qint64 now,qint64 bytes,qint64 captured,qint64 encodeUs) {
    if(m_start<0) m_start=now;
    m_pending.enqueue({id,now,bytes,captured,encodeUs}); m_bytes+=bytes; ++m_frames;
    const auto queue=qMax<qint64>(0,now-captured-encodeUs/1000);m_encodeTotalUs+=encodeUs;m_queueTotalMs+=queue;
    m_encodeMs=m_frames==1 ? encodeUs*.001 : m_encodeMs*.8+encodeUs*.001*.2;m_queueMs=m_frames==1 ? queue : m_queueMs*.8+queue*.2;
}
bool StreamScheduler::acknowledge(quint64 id,qint64 now,double decodeMs) {
    if(m_pending.isEmpty() || (id && m_pending.head().id!=id)) return false;
    const auto frame=m_pending.dequeue(); const double latency=qMax<qint64>(0,now-frame.sent);
    m_ackTotalMs+=qint64(latency);++m_ackedFrames;
    m_ackMs=m_ackMs ? m_ackMs*.8+latency*.2 : latency; m_decodeMs=qBound(0.0,decodeMs,10000.0);
    m_minAckMs=m_minAckMs ? qMin(m_minAckMs,latency) : latency;
    if(m_lastAck>=0 && now>m_lastAck) { const double sample=frame.bytes*1000.0/(now-m_lastAck); m_deliveryBytesPerSecond=m_deliveryBytesPerSecond ? m_deliveryBytesPerSecond*.9+sample*.1 : sample; }
    m_lastAck=now; return true;
}
QJsonObject StreamScheduler::metrics(qint64 now) const {
    return {{"frames",double(m_frames)},{"wireBytes",double(m_bytes)},{"fps",m_start<0 ? 0.0 : m_frames*1000.0/qMax<qint64>(1,now-m_start)},
        {"encodeMs",m_encodeMs},{"ackRttMs",m_ackMs},{"decodeMs",m_decodeMs},{"captureQueueMs",m_queueMs},
        {"pendingFrames",m_pending.size()},{"estimatedDeliveryBps",m_deliveryBytesPerSecond*8},
        {"meanEncodeMs",m_frames ? double(m_encodeTotalUs)/1000/m_frames : 0.0},
        {"meanAckRttMs",m_ackedFrames ? double(m_ackTotalMs)/m_ackedFrames : 0.0},
        {"meanCaptureQueueMs",m_frames ? double(m_queueTotalMs)/m_frames : 0.0}};
}
