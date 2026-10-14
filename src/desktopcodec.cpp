#include "desktopcodec.h"
#include <QBuffer>
#include <QDebug>
#include <QDataStream>
#include <QImageReader>
#include <QtEndian>
#include <cstring>
namespace {
constexpr int Tile=128;
constexpr qint64 MaxPixels=8000000;
enum Kind : quint8 { ImageKey=1,Delta=3,Repeat=4 };
bool validSize(int w,int h) { return w>0 && h>0 && w<=4096 && h<=4096 && qint64(w)*h<=MaxPixels; }
QByteArray header(Kind kind,quint32 base,quint32 sequence,QSize size,quint16 count=0) {
    QByteArray bytes("LDD1",4); QDataStream out(&bytes,QIODevice::Append); out.setVersion(QDataStream::Qt_6_0);
    out<<quint8(kind)<<base<<sequence<<quint16(size.width())<<quint16(size.height())<<count; return bytes;
}
QByteArray encodeImage(const QImage &image,bool lossless,int quality) {
    QByteArray bytes; QBuffer out(&bytes); out.open(QIODevice::WriteOnly); image.save(&out,lossless ? "PNG" : "JPEG",lossless ? -1 : quality); return bytes;
}
bool decodeImage(const QByteArray &bytes,QSize expected,QImage *image) {
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer); reader.setDecideFormatFromContent(true);
    if(reader.size()!=expected) return false;
    *image=reader.read().convertToFormat(QImage::Format_RGBA8888); return !image->isNull();
}
}
void DesktopEncoder::reset() { m_reference={}; m_candidate={}; m_sequence=0; m_candidateSequence=0; }
void DesktopEncoder::commit() { if(!m_candidate.isNull()) { m_reference=m_candidate; m_sequence=m_candidateSequence; m_candidate={}; } }
QByteArray DesktopEncoder::encode(const QImage &source,int quality,bool lossless) {
    if(!validSize(source.width(),source.height())) return {};
    const auto image=source.convertToFormat(QImage::Format_RGBA8888); QList<QRect> dirty; qint64 area=0;
    const bool key=m_reference.size()!=image.size();
    if(!key) for(int y=0;y<image.height();y+=Tile) for(int x=0;x<image.width();x+=Tile) {
        QRect rect(x,y,qMin(Tile,image.width()-x),qMin(Tile,image.height()-y));
        int left=rect.width(),right=-1,top=rect.height(),bottom=-1;
        for(int row=0;row<rect.height();++row) {
            const auto current=image.constScanLine(y+row)+x*4,previous=m_reference.constScanLine(y+row)+x*4;
            if(std::memcmp(current,previous,rect.width()*4)==0) continue;
            top=qMin(top,row); bottom=row;
            int first=0; while(first<left && std::memcmp(current+first*4,previous+first*4,4)==0) ++first; left=qMin(left,first);
            int last=rect.width()-1; while(last>right && std::memcmp(current+last*4,previous+last*4,4)==0) --last; right=qMax(right,last);
        }
        if(bottom>=0) { QRect changed(x+left,y+top,right-left+1,bottom-top+1); dirty.append(changed); area+=qint64(changed.width())*changed.height(); }
    }
    static thread_local int traceCount=0;
    if(qEnvironmentVariableIsSet("LINKDESK_CODEC_TRACE") && traceCount++<12) qInfo()<<"Desktop changes:"<<area<<"pixels, rectangles:"<<dirty.size()<<"key:"<<key;
    m_candidate={};
    if(!key && dirty.isEmpty()) return header(Repeat,m_sequence,m_sequence,image.size());
    const quint32 next=m_sequence+1; m_candidate=image; m_candidateSequence=next;
    if(!lossless && quality<=30) return header(ImageKey,0,next,image.size())+encodeImage(image,false,quality);
    if(!key && area<qint64(image.width())*image.height()*0.45) {
        auto packet=header(Delta,m_sequence,next,image.size(),quint16(dirty.size()));
        QDataStream out(&packet,QIODevice::Append); out.setVersion(QDataStream::Qt_6_0);
        for(const auto &r:dirty) {
            const auto tile=encodeImage(image.copy(r),true,quality);
            if(tile.isEmpty()) return {};
            out<<quint16(r.x())<<quint16(r.y())<<quint16(r.width())<<quint16(r.height())<<quint32(tile.size());
            out.writeRawData(tile.constData(),tile.size());
        }
        // Photographic changes may be cheaper as a whole JPEG than lossless tiles.
        if(!lossless && packet.size()>32*1024) { auto full=encodeImage(image,false,quality); if(!full.isEmpty() && full.size()<packet.size()) return header(ImageKey,0,next,image.size())+full; }
        return packet;
    }
    if(lossless) return header(ImageKey,0,next,image.size())+encodeImage(image,true,quality);
    auto full=encodeImage(image,false,quality);
    if(full.isEmpty()) return {};
    if(key) {
        auto png=encodeImage(image,true,quality);
        if(!png.isEmpty() && (png.size()<=256*1024 || png.size()<=full.size()*1.5)) return header(ImageKey,0,next,image.size())+png;
    }
    return header(ImageKey,0,next,image.size())+full;
}
void DesktopDecoder::reset() { m_image={}; m_sequence=0; }
bool DesktopDecoder::decode(const QByteArray &packet,QImage *image,bool *changed,QString *error) {
    auto fail=[&] { if(error) *error="增量画面格式或参考帧无效。"; return false; };
    if(packet.size()<19 || packet.left(4)!="LDD1") return fail();
    QDataStream in(packet.mid(4)); in.setVersion(QDataStream::Qt_6_0); quint8 kind; quint32 base,sequence; quint16 w,h,count;
    in>>kind>>base>>sequence>>w>>h>>count;
    if(in.status()!=QDataStream::Ok || !validSize(w,h) || count>1024) return fail();
    const auto body=packet.mid(19); const QSize size(w,h); QImage result;
    if(kind==Repeat) {
        if(count || !body.isEmpty() || size!=m_image.size() || base!=m_sequence || sequence!=base || m_image.isNull()) return fail();
        *image=m_image; *changed=false; return true;
    }
    if(kind==ImageKey) {
        if(base || count || !sequence) return fail();
        if(!decodeImage(body,size,&result)) return fail();
    } else if(kind==Delta) {
        if(!count || m_image.size()!=size || m_image.isNull() || base!=m_sequence || sequence!=base+1) return fail();
        QList<QPair<QRect,QImage>> patches; qint64 pixels=0;
        for(int i=0;i<count;++i) {
            quint16 x,y,rw,rh; quint32 length; in>>x>>y>>rw>>rh>>length; QRect r(x,y,rw,rh);
            if(in.status()!=QDataStream::Ok || !rw || !rh || !QRect(QPoint(0,0),size).contains(r) || length>quint32(packet.size()) || length>quint64(in.device()->bytesAvailable())) return fail();
            pixels+=qint64(rw)*rh; if(pixels>MaxPixels) return fail();
            QByteArray bytes(length,Qt::Uninitialized); if(in.readRawData(bytes.data(),length)!=int(length)) return fail();
            QImage tile; if(!decodeImage(bytes,r.size(),&tile)) return fail(); patches.append({r,tile});
        }
        if(!in.atEnd()) return fail();
        result=m_image.copy();
        for(const auto &patch:patches) { const auto &r=patch.first; const auto &tile=patch.second;
            for(int y=0;y<r.height();++y) std::memcpy(result.scanLine(r.y()+y)+r.x()*4,tile.constScanLine(y),r.width()*4);
        }
    } else return fail();
    m_image=result; m_sequence=sequence; *image=m_image; *changed=true; return true;
}
