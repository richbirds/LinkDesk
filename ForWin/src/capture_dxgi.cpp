#include "capture_dxgi.h"
#include <QDateTime>
#include <QDebug>
#include <d3d11.h>
#include <dxgi1_2.h>
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
    void reset() { release(staging); release(duplication); release(context); release(device); last={}; }
    bool initialize(const QRect &wanted) {
        reset(); geometry=wanted;
        D3D_FEATURE_LEVEL level;
        if(FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context))) return false;
        IDXGIDevice *dxgi=nullptr; IDXGIAdapter *adapter=nullptr; IDXGIOutput *output=nullptr; IDXGIOutput1 *output1=nullptr;
        bool ok=false;
        if(SUCCEEDED(device->QueryInterface(IID_IDXGIDevice,reinterpret_cast<void **>(&dxgi))) && SUCCEEDED(dxgi->GetAdapter(&adapter))) {
            for(UINT i=0;adapter->EnumOutputs(i,&output)!=DXGI_ERROR_NOT_FOUND;++i) {
                if(!output) break;
                DXGI_OUTPUT_DESC desc{}; output->GetDesc(&desc);
                const auto r=desc.DesktopCoordinates;
                if(desc.AttachedToDesktop && QRect(r.left,r.top,r.right-r.left,r.bottom-r.top)==wanted && desc.Rotation==DXGI_MODE_ROTATION_IDENTITY) {
                    if(SUCCEEDED(output->QueryInterface(IID_IDXGIOutput1,reinterpret_cast<void **>(&output1)))) ok=SUCCEEDED(output1->DuplicateOutput(device,&duplication));
                    release(output); break;
                }
                release(output);
            }
        }
        release(output1); release(output); release(adapter); release(dxgi); return ok;
    }
public:
    ~DesktopCapture() { reset(); }
    QImage frame(const QRect &wanted,const QSize &maximum) {
        const auto now=QDateTime::currentMSecsSinceEpoch();
        if(!duplication || geometry!=wanted) {
            if(now<retryAfter) return {};
            if(!initialize(wanted)) { reset(); retryAfter=now+5000; return {}; }
        }
        IDXGIResource *resource=nullptr; DXGI_OUTDUPL_FRAME_INFO info{};
        auto hr=duplication->AcquireNextFrame(0,&info,&resource);
        if(hr==DXGI_ERROR_WAIT_TIMEOUT) return last.isNull() ? QImage() : last.scaled(maximum,Qt::KeepAspectRatio,Qt::FastTransformation);
        if(FAILED(hr)) { reset(); retryAfter=now+1000; return {}; }
        ID3D11Texture2D *texture=nullptr; bool ok=false;
        if(resource && SUCCEEDED(resource->QueryInterface(IID_ID3D11Texture2D,reinterpret_cast<void **>(&texture)))) {
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            if(desc.Width==UINT(wanted.width()) && desc.Height==UINT(wanted.height()) && desc.Format==DXGI_FORMAT_B8G8R8A8_UNORM) {
                if(!staging) { desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ; desc.MiscFlags=0; device->CreateTexture2D(&desc,nullptr,&staging); }
                if(staging) {
                    context->CopyResource(staging,texture); D3D11_MAPPED_SUBRESOURCE mapped{};
                    if(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) {
                        last=QImage(static_cast<const uchar *>(mapped.pData),int(desc.Width),int(desc.Height),int(mapped.RowPitch),QImage::Format_RGB32).copy();
                        context->Unmap(staging,0); ok=!last.isNull();
                    }
                }
            }
        }
        release(texture); release(resource); duplication->ReleaseFrame();
        if(!ok) { reset(); retryAfter=now+5000; return {}; }
        if(!reported) { qInfo("Windows capture: DXGI Desktop Duplication active"); reported=true; }
        return last.scaled(maximum,Qt::KeepAspectRatio,Qt::FastTransformation);
    }
};
}
QImage captureDesktopDXGI(const QRect &geometry,const QSize &maximum) {
    if(qEnvironmentVariableIsSet("LINKDESK_DISABLE_DXGI")) return {};
    static thread_local DesktopCapture capture; return capture.frame(geometry,maximum);
}
