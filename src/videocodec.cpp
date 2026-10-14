#include "videocodec.h"
#include <QDataStream>
#include <QIODevice>
QByteArray videoPacket(const QByteArray &data,QSize size,quint32 sequence) {
    if(data.isEmpty() || data.size()>8*1024*1024-16) return {};
    QByteArray packet("LDV1",4); QDataStream stream(&packet,QIODevice::Append); stream<<quint16(size.width())<<quint16(size.height())<<sequence<<quint32(data.size()); packet+=data; return packet;
}
bool unpackVideoPacket(const QByteArray &packet,QByteArray *data,QSize *size,quint32 *sequence,QString *error) {
    auto fail=[&]{if(error) *error="H.264 画面数据无效。"; return false;};
    if(packet.size()<17 || packet.size()>8*1024*1024 || packet.left(4)!="LDV1") return fail();
    QDataStream stream(packet.mid(4)); quint16 w,h; quint32 length; stream>>w>>h>>*sequence>>length;
    if(stream.status()!=QDataStream::Ok || !w || !h || w>4096 || h>4096 || qint64(w)*h>8000000 || (w&1) || (h&1) || !*sequence || length!=quint32(packet.size()-16)) return fail();
    *size=QSize(w,h); *data=packet.mid(16); return true;
}
