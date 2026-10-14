#include "videocodec.h"
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <QElapsedTimer>
#include <QThread>
#include <QLibrary>
#include <cstring>
namespace {
struct MediaApi {
    QLibrary library{"mfplat"};
    decltype(&MFStartup) startup=nullptr;
    decltype(&MFShutdown) shutdown=nullptr;
    decltype(&MFCreateMediaType) createType=nullptr;
    decltype(&MFCreateSample) createSample=nullptr;
    decltype(&MFCreateMemoryBuffer) createBuffer=nullptr;
    decltype(&MFTEnumEx) enumerate=nullptr;
    MediaApi() {
        library.setLoadHints(QLibrary::PreventUnloadHint);
        if(!library.load()) return;
        startup=reinterpret_cast<decltype(startup)>(library.resolve("MFStartup"));
        shutdown=reinterpret_cast<decltype(shutdown)>(library.resolve("MFShutdown"));
        createType=reinterpret_cast<decltype(createType)>(library.resolve("MFCreateMediaType"));
        createSample=reinterpret_cast<decltype(createSample)>(library.resolve("MFCreateSample"));
        createBuffer=reinterpret_cast<decltype(createBuffer)>(library.resolve("MFCreateMemoryBuffer"));
        enumerate=reinterpret_cast<decltype(enumerate)>(library.resolve("MFTEnumEx"));
    }
    bool available() const { return startup && shutdown && createType && createSample && createBuffer && enumerate; }
};
MediaApi &mediaApi() { static MediaApi api;return api; }
// Delay-load the optional Media Foundation runtime so Windows editions without
// the media feature pack can still start and use the PNG/JPEG compatibility path.
#define MFCreateMediaType mediaApi().createType
#define MFCreateSample mediaApi().createSample
#define MFCreateMemoryBuffer mediaApi().createBuffer
#define MFTEnumEx mediaApi().enumerate
template<class T> void release(T *&p) { if(p) { p->Release(); p=nullptr; } }
class Foundation {
    HRESULT com=E_FAIL,media=E_FAIL;
public:
    Foundation() { com=CoInitializeEx(nullptr,COINIT_MULTITHREADED); if(mediaApi().available()) media=mediaApi().startup(MF_VERSION,MFSTARTUP_LITE); }
    ~Foundation() { if(SUCCEEDED(media)) mediaApi().shutdown(); if(SUCCEEDED(com)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(media) && (SUCCEEDED(com) || com==RPC_E_CHANGED_MODE); }
};
void property(IMFTransform *mft,const GUID &key,ULONG value,bool boolean=false) {
    ICodecAPI *codec=nullptr; if(SUCCEEDED(mft->QueryInterface(IID_ICodecAPI,reinterpret_cast<void **>(&codec)))) {
        VARIANT v; VariantInit(&v); v.vt=boolean ? VT_BOOL : VT_UI4;
        if(boolean) v.boolVal=value ? VARIANT_TRUE : VARIANT_FALSE; else v.ulVal=value;
        codec->SetValue(&key,&v); codec->Release();
    }
}
IMFMediaType *type(const GUID &subtype,QSize size,int fps,int bitrate=0) {
    IMFMediaType *t=nullptr; if(FAILED(MFCreateMediaType(&t))) return nullptr;
    t->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video); t->SetGUID(MF_MT_SUBTYPE,subtype);
    MFSetAttributeSize(t,MF_MT_FRAME_SIZE,UINT32(size.width()),UINT32(size.height()));
    MFSetAttributeRatio(t,MF_MT_FRAME_RATE,UINT32(fps),1); MFSetAttributeRatio(t,MF_MT_PIXEL_ASPECT_RATIO,1,1);
    t->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
    t->SetUINT32(MF_MT_YUV_MATRIX,MFVideoTransferMatrix_BT601);
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,MFNominalRange_16_235);
    if(bitrate) { t->SetUINT32(MF_MT_AVG_BITRATE,UINT32(bitrate)); t->SetUINT32(MF_MT_MPEG2_PROFILE,77); }
    return t;
}
class Transform {
public:
    Foundation foundation;
    IMFTransform *mft=nullptr; IMFMediaEventGenerator *events=nullptr;
    bool hardware=false,inputReady=false,outputReady=false;
    DWORD inputId=0,outputId=0;
    HRESULT lastResult=S_OK;
    ~Transform() { close(); }
    void close() { if(mft) { mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH,0); mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING,0); } release(events); release(mft); inputReady=outputReady=false; }
    bool open(bool encoder) {
        if(!foundation.ok() || qEnvironmentVariableIsSet("LINKDESK_DISABLE_H264")) return false;
        MFT_REGISTER_TYPE_INFO input{MFMediaType_Video,MFVideoFormat_H264};
        MFT_REGISTER_TYPE_INFO output{MFMediaType_Video,MFVideoFormat_H264};
        IMFActivate **activations=nullptr; UINT32 count=0;
        const UINT32 flags=encoder ? MFT_ENUM_FLAG_HARDWARE|MFT_ENUM_FLAG_SORTANDFILTER : MFT_ENUM_FLAG_SYNCMFT|MFT_ENUM_FLAG_LOCALMFT|MFT_ENUM_FLAG_SORTANDFILTER;
        HRESULT hr=MFTEnumEx(encoder ? MFT_CATEGORY_VIDEO_ENCODER : MFT_CATEGORY_VIDEO_DECODER,flags,encoder ? nullptr : &input,encoder ? &output : nullptr,&activations,&count);
        if(SUCCEEDED(hr)) for(UINT32 i=0;i<count;++i) {
            if(!mft && SUCCEEDED(activations[i]->ActivateObject(IID_PPV_ARGS(&mft)))) hardware=encoder;
            activations[i]->Release();
        }
        CoTaskMemFree(activations); if(!mft) return false;
        IMFAttributes *attributes=nullptr;
        if(SUCCEEDED(mft->GetAttributes(&attributes))) {
            attributes->SetUINT32(MF_LOW_LATENCY,TRUE);
            UINT32 async=0; attributes->GetUINT32(MF_TRANSFORM_ASYNC,&async);
            if(async) { attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK,TRUE); mft->QueryInterface(IID_PPV_ARGS(&events)); }
            attributes->Release();
        }
        DWORD in=0,out=0; if(SUCCEEDED(mft->GetStreamIDs(1,&in,1,&out))) { inputId=in; outputId=out; }
        return true;
    }
    void begin() { mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING,0); mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM,0); }
    void poll() {
        if(!events) return;
        IMFMediaEvent *event=nullptr;
        while(SUCCEEDED(events->GetEvent(MF_EVENT_FLAG_NO_WAIT,&event))) {
            MediaEventType kind=MEUnknown; event->GetType(&kind);
            if(kind==METransformNeedInput) inputReady=true;
            if(kind==METransformHaveOutput) outputReady=true;
            event->Release(); event=nullptr;
        }
    }
    bool wait(bool input) {
        if(!events) return true;
        QElapsedTimer deadline; deadline.start();
        do { poll(); if(input ? inputReady : outputReady) return true; QThread::msleep(1); } while(deadline.elapsed()<100);
        return false;
    }
    bool push(const QByteArray &data,qint64 timestamp,qint64 duration) {
        if(!wait(true)) return false;
        IMFMediaBuffer *buffer=nullptr; IMFSample *sample=nullptr;
        if(FAILED(MFCreateMemoryBuffer(DWORD(data.size()),&buffer)) || FAILED(MFCreateSample(&sample))) { release(buffer); release(sample); return false; }
        BYTE *bytes=nullptr; HRESULT hr=buffer->Lock(&bytes,nullptr,nullptr);
        if(SUCCEEDED(hr)) { memcpy(bytes,data.constData(),size_t(data.size())); buffer->Unlock(); buffer->SetCurrentLength(DWORD(data.size())); sample->AddBuffer(buffer); sample->SetSampleTime(timestamp); sample->SetSampleDuration(duration); hr=mft->ProcessInput(inputId,sample,0); }
        release(buffer); release(sample); inputReady=false; return SUCCEEDED(hr);
    }
    QByteArray pull(QSize size,bool *streamChange=nullptr) {
        if(!wait(false)) return {};
        MFT_OUTPUT_STREAM_INFO info{}; if(FAILED(mft->GetOutputStreamInfo(outputId,&info))) return {};
        IMFSample *owned=nullptr; IMFMediaBuffer *buffer=nullptr;
        if(!(info.dwFlags&MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
            if(FAILED(MFCreateSample(&owned)) || FAILED(MFCreateMemoryBuffer(qMax<DWORD>(info.cbSize,DWORD(size.width()*size.height()*4)),&buffer))) { release(owned); release(buffer); return {}; }
            owned->AddBuffer(buffer); release(buffer);
        }
        MFT_OUTPUT_DATA_BUFFER output{}; output.dwStreamID=outputId; output.pSample=owned; DWORD status=0;
        const HRESULT hr=mft->ProcessOutput(0,1,&output,&status); lastResult=hr;outputReady=false;
        if(streamChange) *streamChange=hr==MF_E_TRANSFORM_STREAM_CHANGE;
        QByteArray result;
        if(SUCCEEDED(hr) && output.pSample && SUCCEEDED(output.pSample->ConvertToContiguousBuffer(&buffer))) {
            BYTE *bytes=nullptr; DWORD length=0;
            if(SUCCEEDED(buffer->Lock(&bytes,nullptr,&length))) { result=QByteArray(reinterpret_cast<char *>(bytes),int(length)); buffer->Unlock(); }
            release(buffer);
        }
        if(output.pSample && output.pSample!=owned) output.pSample->Release(); release(owned); release(output.pEvents); return result;
    }
};
int clamp(int v) { return qBound(0,v,255); }
QByteArray nv12(const QImage &source) {
    const auto image=source.convertToFormat(QImage::Format_RGB32); const int w=image.width(),h=image.height();
    QByteArray bytes(w*h*3/2,Qt::Uninitialized); auto yplane=reinterpret_cast<uchar *>(bytes.data()),uv=yplane+w*h;
    for(int y=0;y<h;++y) { const auto row=reinterpret_cast<const QRgb *>(image.constScanLine(y));for(int x=0;x<w;++x) { const QRgb p=row[x]; yplane[y*w+x]=uchar(clamp(((66*qRed(p)+129*qGreen(p)+25*qBlue(p)+128)>>8)+16)); } }
    for(int y=0;y<h;y+=2) { const auto row0=reinterpret_cast<const QRgb *>(image.constScanLine(y)),row1=reinterpret_cast<const QRgb *>(image.constScanLine(y+1));for(int x=0;x<w;x+=2) { int r=0,g=0,b=0;for(const auto p:{row0[x],row0[x+1],row1[x],row1[x+1]}) { r+=qRed(p);g+=qGreen(p);b+=qBlue(p); } r/=4;g/=4;b/=4; uv[y/2*w+x]=uchar(clamp(((-38*r-74*g+112*b+128)>>8)+128)); uv[y/2*w+x+1]=uchar(clamp(((112*r-94*g-18*b+128)>>8)+128)); } }
    return bytes;
}
QImage rgb(const QByteArray &bytes,QSize size,QSize coded,int stride,bool bt709,bool fullRange) {
    const int w=size.width(),h=size.height();
    if(stride<w || stride>16384 || coded.width()<w || coded.width()>((w+15)&~15) || coded.height()<h || coded.height()>((h+15)&~15) || (coded.height()&1) || bytes.size()<qint64(stride)*coded.height()*3/2) return {};
    QImage image(size,QImage::Format_RGB32); const auto data=reinterpret_cast<const uchar *>(bytes.constData());
    // MF output can have padded rows; derive stride from the NV12 sample length.
    if(stride<w || stride>16384) return {};
    const int cy=fullRange ? 256 : 298;
    const int cr=bt709 ? (fullRange ? 403 : 459) : (fullRange ? 359 : 409);
    const int cgU=bt709 ? (fullRange ? 48 : 55) : (fullRange ? 88 : 100);
    const int cgV=bt709 ? (fullRange ? 120 : 136) : (fullRange ? 183 : 208);
    const int cb=bt709 ? (fullRange ? 475 : 541) : (fullRange ? 454 : 516);
    for(int y=0;y<h;++y) { auto row=reinterpret_cast<QRgb *>(image.scanLine(y)); for(int x=0;x<w;++x) { const int yy=qMax(0,int(data[y*stride+x])-(fullRange ? 0 : 16)),u=int(data[stride*coded.height()+(y/2)*stride+(x&~1)])-128,v=int(data[stride*coded.height()+(y/2)*stride+(x&~1)+1])-128; row[x]=qRgb(clamp((cy*yy+cr*v+128)>>8),clamp((cy*yy-cgU*u-cgV*v+128)>>8),clamp((cy*yy+cb*u+128)>>8)); } }
    return image;
}
class Encoder final : public VideoEncoder {
    std::unique_ptr<Transform> t; VideoSettings previous; quint64 frame=0;
public:
    bool hardware() const override { return t && t->hardware; }
    void reset() override { t.reset(); frame=0; }
    QByteArray encode(const QImage &image,const VideoSettings &settings,QString *error) override {
        if(image.size()!=settings.size || (image.width()&1) || (image.height()&1)) return {};
        if(!t || previous.size!=settings.size || previous.fps!=settings.fps) {
            reset(); t=std::make_unique<Transform>();
            if(!t->open(true)) { if(error) *error="Windows 硬件 H.264 编码器不可用，使用增量图像。"; reset(); return {}; }
            property(t->mft,CODECAPI_AVLowLatencyMode,1,true); property(t->mft,CODECAPI_AVEncMPVDefaultBPictureCount,0);
            auto output=type(MFVideoFormat_H264,settings.size,settings.fps,settings.bitrate),input=type(MFVideoFormat_NV12,settings.size,settings.fps);
            const bool ok=output && input && SUCCEEDED(t->mft->SetOutputType(t->outputId,output,0)) && SUCCEEDED(t->mft->SetInputType(t->inputId,input,0)); release(output); release(input);
            if(!ok) { if(error) *error="硬件编码器不支持此画面格式，使用增量图像。"; reset(); return {}; } t->begin(); previous=settings;
        }
        property(t->mft,CODECAPI_AVEncCommonMeanBitRate,ULONG(settings.bitrate));
        if(!frame) property(t->mft,CODECAPI_AVEncVideoForceKeyFrame,1,true);
        const qint64 duration=10000000/qMax(1,settings.fps);
        if(!t->push(nv12(image),qint64(frame)*duration,duration)) { if(error) *error="硬件编码输入未就绪。"; reset(); return {}; }
        auto data=t->pull(settings.size);
        if(data.isEmpty()) { if(error) *error="硬件编码未及时输出，使用增量图像。"; reset(); return {}; }
        if(!frame) {
            IMFMediaType *current=nullptr;
            if(SUCCEEDED(t->mft->GetOutputCurrentType(t->outputId,&current))) {
                UINT32 length=0; if(SUCCEEDED(current->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER,&length)) && length && length<65536) { QByteArray header(int(length),Qt::Uninitialized); if(SUCCEEDED(current->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER,reinterpret_cast<UINT8 *>(header.data()),length,nullptr))) data.prepend(header); } current->Release();
            }
        }
        ++frame; return data;
    }
};
class Decoder final : public VideoDecoder {
    std::unique_ptr<Transform> t; QSize previous;
public:
    void reset() override { t.reset(); previous={}; }
    QImage decode(const QByteArray &bytes,QSize size,QString *error) override {
        if(!t || previous!=size) {
            reset(); t=std::make_unique<Transform>(); if(!t->open(false)) { if(error) *error="H.264 解码器不可用。"; reset(); return {}; }
            property(t->mft,CODECAPI_AVLowLatencyMode,1,true);
            property(t->mft,CODECAPI_AVDecVideoMaxCodedWidth,ULONG((size.width()+15)&~15));property(t->mft,CODECAPI_AVDecVideoMaxCodedHeight,ULONG((size.height()+15)&~15));
            auto input=type(MFVideoFormat_H264,size,30),output=type(MFVideoFormat_NV12,size,30);
            const bool ok=input && output && SUCCEEDED(t->mft->SetInputType(t->inputId,input,0)) && SUCCEEDED(t->mft->SetOutputType(t->outputId,output,0)); release(input); release(output);
            if(!ok) { if(error) *error="H.264 解码格式不支持。"; reset(); return {}; } previous=size; t->begin();
        }
        if(!t->push(bytes,0,333333)) { if(error) *error="H.264 解码输入失败。"; reset(); return {}; }
        bool changed=false; auto decoded=t->pull(size,&changed);
        QString formats;
        if(changed) {
            bool selected=false;
            for(DWORD i=0;i<32;++i) { IMFMediaType *output=nullptr;if(FAILED(t->mft->GetOutputAvailableType(t->outputId,i,&output))) break;
                GUID subtype{};UINT32 w=0,h=0;output->GetGUID(MF_MT_SUBTYPE,&subtype);MFGetAttributeSize(output,MF_MT_FRAME_SIZE,&w,&h);
                formats+=QString(" %1:%2x%3").arg(subtype.Data1,8,16,QChar('0')).arg(w).arg(h);
                if(subtype==MFVideoFormat_NV12 && w>=UINT32(size.width()) && w<=UINT32((size.width()+15)&~15) && h>=UINT32(size.height()) && h<=UINT32((size.height()+15)&~15) && SUCCEEDED(t->mft->SetOutputType(t->outputId,output,0))) selected=true;
                release(output);if(selected) break;
            }
            if(selected) decoded=t->pull(size);
        }
        QSize coded=size;int stride=size.width();IMFMediaType *current=nullptr;bool bt709=false,fullRange=false;
        if(SUCCEEDED(t->mft->GetOutputCurrentType(t->outputId,&current))) { UINT32 w=0,h=0,pitch=0;MFGetAttributeSize(current,MF_MT_FRAME_SIZE,&w,&h);if(w && h) coded=QSize(int(w),int(h));if(SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE,&pitch))) stride=int(pitch);else if(!decoded.isEmpty()) stride=int(decoded.size()/(coded.height()*3/2));current->Release(); }
        if(SUCCEEDED(t->mft->GetOutputCurrentType(t->outputId,&current))) { UINT32 matrix=0,range=0;current->GetUINT32(MF_MT_YUV_MATRIX,&matrix);current->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE,&range);bt709=matrix==MFVideoTransferMatrix_BT709;fullRange=range==MFNominalRange_0_255;current->Release(); }
        auto image=rgb(decoded,size,coded,stride,bt709,fullRange); if(image.isNull() && error) *error=QString("H.264 解码未返回有效画面 (HRESULT %1, output %2 bytes; formats%3)。").arg(quint32(t->lastResult),8,16,QChar('0')).arg(decoded.size()).arg(formats); return image;
    }
};
}
std::unique_ptr<VideoEncoder> createVideoEncoder() { return std::make_unique<Encoder>(); }
std::unique_ptr<VideoDecoder> createVideoDecoder() { return std::make_unique<Decoder>(); }
