#pragma once
#include <QImage>
#include <QByteArray>
#include <QString>
#include <memory>
struct VideoSettings { QSize size; int fps=30; int bitrate=4000000; };
class VideoEncoder {
public:
    virtual ~VideoEncoder()=default;
    virtual QByteArray encode(const QImage &,const VideoSettings &,QString *error)=0;
    virtual void reset()=0;
    virtual bool hardware() const=0;
};
class VideoDecoder {
public:
    virtual ~VideoDecoder()=default;
    virtual QImage decode(const QByteArray &,QSize,QString *error)=0;
    virtual void reset()=0;
};
std::unique_ptr<VideoEncoder> createVideoEncoder();
std::unique_ptr<VideoDecoder> createVideoDecoder();
QByteArray videoPacket(const QByteArray &annexB,QSize size,quint32 sequence);
bool unpackVideoPacket(const QByteArray &,QByteArray *annexB,QSize *,quint32 *,QString *error);
