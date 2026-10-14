#include "videocodec.h"
#include <VideoToolbox/VideoToolbox.h>
#include <CoreVideo/CoreVideo.h>
#include <QtEndian>
#include <cstring>
namespace {
QByteArray annex(CMSampleBufferRef sample) {
    QByteArray result;
    const auto description=CMSampleBufferGetFormatDescription(sample);
    const auto attachments=CMSampleBufferGetSampleAttachmentsArray(sample,false);
    bool key=true;
    if(attachments && CFArrayGetCount(attachments)) key=!CFDictionaryContainsKey(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments,0)),kCMSampleAttachmentKey_NotSync);
    if(key) for(size_t i=0;i<2;++i) { const uint8_t *data=nullptr; size_t size=0,count=0; int length=0;
        if(CMVideoFormatDescriptionGetH264ParameterSetAtIndex(description,i,&data,&size,&count,&length)==noErr) { result.append("\0\0\0\1",4); result.append(reinterpret_cast<const char *>(data),qsizetype(size)); }
    }
    const auto buffer=CMSampleBufferGetDataBuffer(sample); if(!buffer) return {};
    const size_t size=CMBlockBufferGetDataLength(buffer); if(size>8*1024*1024) return {};
    QByteArray bytes(qsizetype(size),Qt::Uninitialized); if(CMBlockBufferCopyDataBytes(buffer,0,size,bytes.data())!=noErr) return {};
    qsizetype offset=0;
    while(offset+4<=bytes.size()) { auto length=qFromBigEndian<quint32>(bytes.constData()+offset); offset+=4; if(!length || length>quint32(bytes.size()-offset)) return {}; result.append("\0\0\0\1",4); result.append(bytes.constData()+offset,length); offset+=length; }
    return offset==bytes.size() ? result : QByteArray();
}
class Encoder final : public VideoEncoder {
    VTCompressionSessionRef session=nullptr; VideoSettings previous; qint64 frame=0; QByteArray output; OSStatus callbackStatus=noErr;
    static void callback(void *owner,void *,OSStatus status,VTEncodeInfoFlags,CMSampleBufferRef sample) { auto self=static_cast<Encoder *>(owner); self->callbackStatus=status; if(status==noErr && sample && CMSampleBufferDataIsReady(sample)) self->output=annex(sample); }
public:
    ~Encoder() override { reset(); }
    bool hardware() const override { return session!=nullptr; }
    void reset() override { if(session) { VTCompressionSessionInvalidate(session); CFRelease(session); session=nullptr; } frame=0; output.clear(); }
    QByteArray encode(const QImage &source,const VideoSettings &settings,QString *error) override {
        if(qEnvironmentVariableIsSet("LINKDESK_DISABLE_H264")) { if(error) *error="硬件视频已禁用，使用增量图像。";reset();return {}; }
        if(source.size()!=settings.size || (source.width()&1) || (source.height()&1)) { if(error) *error="视频画面尺寸无效。";return {}; }
        if(!session || previous.size!=settings.size || previous.fps!=settings.fps) {
            reset();
            const void *keys[]={kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder}; const void *values[]={kCFBooleanTrue};
            auto specification=CFDictionaryCreate(kCFAllocatorDefault,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            const auto status=VTCompressionSessionCreate(kCFAllocatorDefault,settings.size.width(),settings.size.height(),kCMVideoCodecType_H264,specification,nullptr,nullptr,callback,this,&session); CFRelease(specification);
            if(status!=noErr) { if(error) *error="VideoToolbox 硬件编码不可用，使用增量图像。"; reset(); return {}; }
            VTSessionSetProperty(session,kVTCompressionPropertyKey_RealTime,kCFBooleanTrue);
            VTSessionSetProperty(session,kVTCompressionPropertyKey_AllowFrameReordering,kCFBooleanFalse);
            VTSessionSetProperty(session,kVTCompressionPropertyKey_ProfileLevel,kVTProfileLevel_H264_Main_AutoLevel);
            int delay=1; auto number=CFNumberCreate(nullptr,kCFNumberIntType,&delay); VTSessionSetProperty(session,kVTCompressionPropertyKey_MaxFrameDelayCount,number); CFRelease(number);
            VTCompressionSessionPrepareToEncodeFrames(session); previous=settings;
        }
        int bitrate=settings.bitrate; auto number=CFNumberCreate(nullptr,kCFNumberIntType,&bitrate); VTSessionSetProperty(session,kVTCompressionPropertyKey_AverageBitRate,number); CFRelease(number);
        CVPixelBufferRef pixels=nullptr;
        if(CVPixelBufferCreate(nullptr,settings.size.width(),settings.size.height(),kCVPixelFormatType_32BGRA,nullptr,&pixels)!=kCVReturnSuccess) return {};
        const auto image=source.convertToFormat(QImage::Format_RGB32); CVPixelBufferLockBaseAddress(pixels,0);
        auto base=static_cast<uchar *>(CVPixelBufferGetBaseAddress(pixels)); const auto stride=CVPixelBufferGetBytesPerRow(pixels);
        for(int y=0;y<image.height();++y) memcpy(base+y*stride,image.constScanLine(y),image.width()*4);
        CVPixelBufferUnlockBaseAddress(pixels,0); output.clear(); callbackStatus=noErr;
        const auto status=VTCompressionSessionEncodeFrame(session,pixels,CMTimeMake(frame,settings.fps),CMTimeMake(1,settings.fps),nullptr,nullptr,nullptr);
        CVPixelBufferRelease(pixels); VTCompressionSessionCompleteFrames(session,kCMTimeInvalid);
        if(status!=noErr || callbackStatus!=noErr || output.isEmpty()) { if(error) *error="VideoToolbox 编码失败，使用增量图像。"; reset(); return {}; }
        ++frame; return output;
    }
};
class Decoder final : public VideoDecoder {
    VTDecompressionSessionRef session=nullptr; CMVideoFormatDescriptionRef format=nullptr; QByteArray sps,pps; QImage output;
    static void callback(void *owner,void *,OSStatus status,VTDecodeInfoFlags,CVImageBufferRef pixels,CMTime,CMTime) {
        if(status!=noErr || !pixels) return;
        CVPixelBufferLockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);
        const QImage image(static_cast<const uchar *>(CVPixelBufferGetBaseAddress(pixels)),int(CVPixelBufferGetWidth(pixels)),int(CVPixelBufferGetHeight(pixels)),qsizetype(CVPixelBufferGetBytesPerRow(pixels)),QImage::Format_RGB32);
        static_cast<Decoder *>(owner)->output=image.copy(); CVPixelBufferUnlockBaseAddress(pixels,kCVPixelBufferLock_ReadOnly);
    }
public:
    ~Decoder() override { reset(); }
    void reset() override { if(session) { VTDecompressionSessionInvalidate(session); CFRelease(session); session=nullptr; } if(format) { CFRelease(format); format=nullptr; } sps.clear();pps.clear();output={}; }
    QImage decode(const QByteArray &data,QSize size,QString *error) override {
        QList<QByteArray> units; qsizetype start=-1;
        for(qsizetype i=0;i+3<data.size();++i) if(data[i]==0 && data[i+1]==0 && ((data[i+2]==1) || (data[i+2]==0 && data[i+3]==1))) { if(start>=0 && i>start) units.append(data.mid(start,i-start)); i+=data[i+2]==1 ? 2 : 3; start=i+1; }
        if(start>=0 && start<data.size()) units.append(data.mid(start));
        QByteArray avcc,newSps=sps,newPps=pps;
        for(const auto &unit:units) { if(unit.isEmpty()) continue; const auto kind=quint8(unit[0])&31; if(kind==7) newSps=unit; else if(kind==8) newPps=unit; else { QByteArray length(4,'\0'); qToBigEndian<quint32>(quint32(unit.size()),length.data()); avcc+=length;avcc+=unit; } }
        if(newSps.isEmpty() || newPps.isEmpty() || avcc.isEmpty()) { if(error) *error="H.264 参数集缺失。"; return {}; }
        if(!session || newSps!=sps || newPps!=pps) {
            reset();sps=newSps;pps=newPps;
            const uint8_t *params[]={reinterpret_cast<const uint8_t *>(sps.constData()),reinterpret_cast<const uint8_t *>(pps.constData())}; const size_t sizes[]={size_t(sps.size()),size_t(pps.size())};
            if(CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr,2,params,sizes,4,&format)!=noErr) return {};
            const auto dimensions=CMVideoFormatDescriptionGetDimensions(format);
            if(dimensions.width!=size.width() || dimensions.height!=size.height()) { reset(); if(error) *error="H.264 参数集尺寸不匹配。"; return {}; }
            int pixelFormat=kCVPixelFormatType_32BGRA; auto number=CFNumberCreate(nullptr,kCFNumberIntType,&pixelFormat);
            const void *keys[]={kCVPixelBufferPixelFormatTypeKey}; const void *values[]={number}; auto attributes=CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
            VTDecompressionOutputCallbackRecord record{callback,this}; const auto status=VTDecompressionSessionCreate(nullptr,format,nullptr,attributes,&record,&session); CFRelease(number);CFRelease(attributes);
            if(status!=noErr) { reset(); return {}; }
        }
        CMBlockBufferRef block=nullptr; CMSampleBufferRef sample=nullptr;
        if(CMBlockBufferCreateWithMemoryBlock(nullptr,nullptr,size_t(avcc.size()),nullptr,nullptr,0,size_t(avcc.size()),0,&block)!=noErr) return {};
        CMBlockBufferReplaceDataBytes(avcc.constData(),block,0,size_t(avcc.size())); const size_t sampleSize=size_t(avcc.size());
        const auto status=CMSampleBufferCreateReady(nullptr,block,format,1,0,nullptr,1,&sampleSize,&sample); CFRelease(block); if(status!=noErr) return {};
        output={}; const auto decoded=VTDecompressionSessionDecodeFrame(session,sample,kVTDecodeFrame_EnableAsynchronousDecompression,nullptr,nullptr); VTDecompressionSessionWaitForAsynchronousFrames(session); CFRelease(sample);
        if(decoded!=noErr || output.size()!=size) { if(error) *error="VideoToolbox 解码失败。"; return {}; } return output;
    }
};
}
std::unique_ptr<VideoEncoder> createVideoEncoder() { return std::make_unique<Encoder>(); }
std::unique_ptr<VideoDecoder> createVideoDecoder() { return std::make_unique<Decoder>(); }
