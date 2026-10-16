#include "capture_dxgi.h"
#include <QDateTime>
#include <QDebug>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstring>
namespace {
template<class T> void release(T *&p) { if(p) { p->Release(); p=nullptr; } }
class DesktopCapture {
    ID3D11Device *device=nullptr;
    ID3D11DeviceContext *context=nullptr;
    IDXGIOutputDuplication *duplication=nullptr;
    ID3D11Texture2D *staging=nullptr;
    QImage last;
    QRect geometry;
    qint64 retryAfter=0;
    bool reported=false;
    quint64 serial=0;
    CaptureFrame metadata;
    QString lastFailure;
    void reset() { release(staging); release(duplication); release(context); release(device); last={}; }
    bool initialize(const QRect &wanted) {
        reset(); geometry=wanted;
        IDXGIFactory1 *factory=nullptr;
        HRESULT failure=CreateDXGIFactory1(IID_IDXGIFactory1,reinterpret_cast<void **>(&factory));
        const bool factoryReady=SUCCEEDED(failure);
        bool matched=false,ok=false;
        if(SUCCEEDED(failure)) {
            IDXGIAdapter1 *adapter=nullptr;
            for(UINT i=0;!ok && SUCCEEDED(factory->EnumAdapters1(i,&adapter));++i) {
                IDXGIOutput *output=nullptr;
                for(UINT j=0;!ok && SUCCEEDED(adapter->EnumOutputs(j,&output));++j) {
                    DXGI_OUTPUT_DESC desc{};
                    if(SUCCEEDED(output->GetDesc(&desc))) {
                        const auto r=desc.DesktopCoordinates;
                        if(desc.AttachedToDesktop && QRect(r.left,r.top,r.right-r.left,r.bottom-r.top)==wanted && desc.Rotation==DXGI_MODE_ROTATION_IDENTITY) {
                            matched=true;D3D_FEATURE_LEVEL level;
                            // The device must belong to the adapter owning this output,
                            // including an indirect/virtual display on another adapter.
                            release(context);release(device);
                            failure=D3D11CreateDevice(adapter,D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context);
                            IDXGIOutput1 *output1=nullptr;
                            if(SUCCEEDED(failure)) failure=output->QueryInterface(IID_IDXGIOutput1,reinterpret_cast<void **>(&output1));
                            if(SUCCEEDED(failure)) failure=output1->DuplicateOutput(device,&duplication);
                            ok=SUCCEEDED(failure);release(output1);
                        }
                    }
                    release(output);
                }
                release(adapter);
            }
        }
        release(factory);
        if(!ok) lastFailure=(!factoryReady || matched) ? QString("DXGI 初始化失败（0x%1）").arg(quint32(failure),8,16,QChar('0')) : "DXGI 未找到与当前桌面匹配的活动显示输出";
        else lastFailure.clear();
        return ok;
    }
public:
    ~DesktopCapture() { reset(); }
    void clear() { reset();retryAfter=0;geometry={};metadata={};serial=0;lastFailure.clear(); }
    QImage frame(const QRect &wanted,const QSize &maximum) {
        metadata={}; metadata.previousSerial=serial;
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(!duplication || geometry!=wanted) {
            if(now<retryAfter) return {};
            if(!initialize(wanted)) { reset(); retryAfter=now+5000; return {}; }
        }
        IDXGIResource *resource=nullptr; DXGI_OUTDUPL_FRAME_INFO info{};
        auto hr=duplication->AcquireNextFrame(0,&info,&resource);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT) { metadata.unchanged=true; metadata.serial=serial; return last.isNull() ? QImage() : last.scaled(maximum,Qt::KeepAspectRatio,Qt::FastTransformation); }
        if(FAILED(hr)) { lastFailure=QString("DXGI 取帧失败（0x%1）").arg(quint32(hr),8,16,QChar('0')); reset(); retryAfter=now+1000; return {}; }
        if(!info.LastPresentTime.QuadPart && !last.isNull()) { release(resource);duplication->ReleaseFrame();metadata.unchanged=true;metadata.serial=serial;return last.scaled(maximum,Qt::KeepAspectRatio,Qt::FastTransformation); }
        if(info.TotalMetadataBufferSize && !last.isNull()) {
            QByteArray buffer(int(info.TotalMetadataBufferSize),Qt::Uninitialized); UINT used=0;
            if(SUCCEEDED(duplication->GetFrameMoveRects(UINT(buffer.size()),reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT *>(buffer.data()),&used))) {
                const auto rects=reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT *>(buffer.constData());
                for(UINT i=0;i<used/sizeof(*rects);++i) { const auto &r=rects[i]; metadata.moves.append({QPoint(r.SourcePoint.x,r.SourcePoint.y),QRect(r.DestinationRect.left,r.DestinationRect.top,r.DestinationRect.right-r.DestinationRect.left,r.DestinationRect.bottom-r.DestinationRect.top)}); }
                if(SUCCEEDED(duplication->GetFrameDirtyRects(UINT(buffer.size()),reinterpret_cast<RECT *>(buffer.data()),&used))) {
                    const auto dirty=reinterpret_cast<const RECT *>(buffer.constData());
                    for(UINT i=0;i<used/sizeof(*dirty);++i) metadata.dirty+=QRect(dirty[i].left,dirty[i].top,dirty[i].right-dirty[i].left,dirty[i].bottom-dirty[i].top);
                    metadata.regionsValid=true;
                }
            }
        }
        ID3D11Texture2D *texture=nullptr; bool ok=false;
        if(resource && SUCCEEDED(resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void **>(&texture)))) {
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            if(desc.Width==UINT(wanted.width()) && desc.Height==UINT(wanted.height()) && desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM) {
                if(!staging) { desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0; device->CreateTexture2D(&desc,nullptr,&staging); }
                if(staging) {
                    QRegion readback=metadata.dirty;
                    const QRect bounds(QPoint(0,0),wanted.size());
                    for(const auto &move:metadata.moves) { readback+=move.destination; if(!bounds.contains(move.destination) || !bounds.contains(QRect(move.source,move.destination.size()))) metadata.regionsValid=false; }
                    if(metadata.regionsValid) for(const auto &r:readback) if(!bounds.contains(r)) metadata.regionsValid=false;
                    if(metadata.regionsValid) {
                        for(const auto &r:readback) { D3D11_BOX box{UINT(r.left()),UINT(r.top()),0,UINT(r.x()+r.width()),UINT(r.y()+r.height()),1}; context->CopySubresourceRegion(staging,0,UINT(r.x()),UINT(r.y()),0,texture,0,&box); }
                    } else context->CopyResource(staging,texture);
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) {
                        if(metadata.regionsValid) {
                            last.detach();
                            for(const auto &r:readback) for(int y=r.top();y<r.y()+r.height();++y) std::memcpy(last.scanLine(y)+r.x()*4,static_cast<const uchar *>(mapped.pData)+y*mapped.RowPitch+r.x()*4,r.width()*4);
                        } else last=QImage(static_cast<const uchar *>(mapped.pData),int(desc.Width),int(desc.Height),int(mapped.RowPitch),QImage::Format_RGB32).copy();
                        context->Unmap(staging,0); ok=!last.isNull();
                    }
                }
            }
        }
        release(texture); release(resource); duplication->ReleaseFrame();
        if(!ok) { lastFailure="DXGI 画面读取失败"; reset(); retryAfter=now+5000; return {}; }
        metadata.serial=++serial;
        if(last.size()!=last.size().scaled(maximum,Qt::KeepAspectRatio)) metadata.regionsValid=false;
        if(!reported) { qInfo("Windows capture: DXGI Desktop Duplication active"); reported=true; }
        return last.scaled(maximum,Qt::KeepAspectRatio,Qt::FastTransformation);
    }
    CaptureFrame capture(const QRect &geometry,const QSize &maximum) { const auto image=frame(geometry,maximum); auto result=metadata; result.image=image; if(image.isNull()) result.waitReason=lastFailure; return result; }
};
DesktopCapture &captureState() {static thread_local DesktopCapture capture;return capture;}
}
void resetDesktopCaptureDXGI() {captureState().clear();}
QImage captureDesktopDXGI(const QRect &geometry,const QSize &maximum) {
    return captureDesktopFrameDXGI(geometry,maximum).image;
}
CaptureFrame captureDesktopFrameDXGI(const QRect &geometry,const QSize &maximum) {
    if(qEnvironmentVariableIsSet("LINKDESK_DISABLE_DXGI")) return {};
    return captureState().capture(geometry,maximum);
}
