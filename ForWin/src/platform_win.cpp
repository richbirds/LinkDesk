#include "capture_dxgi.h"
#include "platform.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#include <QSaveFile>
#include <QFile>
#include <QScreen>
#include <QCoreApplication>
#include <QSslSocket>

namespace {
QByteArray pem(const char *label,const QByteArray &der) {
    QByteArray result="-----BEGIN "+QByteArray(label)+"-----\n";
    auto encoded=der.toBase64();
    for (int i=0;i<encoded.size();i+=64) result+=encoded.mid(i,64)+"\n";
    return result+"-----END "+QByteArray(label)+"-----\n";
}
bool save(const QString &path,const QByteArray &data) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
    return file.write(data)==data.size() && file.commit();
}
QRect primaryRect() {
    MONITORINFO info{}; info.cbSize=sizeof(info);
    if (!GetMonitorInfoW(MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY),&info)) return {};
    const auto r=info.rcMonitor; return QRect(r.left,r.top,r.right-r.left,r.bottom-r.top);
}
}

bool Platform::generateIdentity(const QString &certPath,const QString &keyPath,QString *error) {
    NCRYPT_PROV_HANDLE provider=0; NCRYPT_KEY_HANDLE key=0; PCCERT_CONTEXT cert=nullptr;
    SECURITY_STATUS status=NCryptOpenStorageProvider(&provider,MS_KEY_STORAGE_PROVIDER,0);
    if (status==ERROR_SUCCESS) status=NCryptCreatePersistedKey(provider,&key,NCRYPT_RSA_ALGORITHM,nullptr,0,0);
    DWORD bits=2048, policy=NCRYPT_ALLOW_EXPORT_FLAG|NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
    if (status==ERROR_SUCCESS) status=NCryptSetProperty(key,NCRYPT_LENGTH_PROPERTY,reinterpret_cast<PBYTE>(&bits),sizeof(bits),0);
    if (status==ERROR_SUCCESS) status=NCryptSetProperty(key,NCRYPT_EXPORT_POLICY_PROPERTY,reinterpret_cast<PBYTE>(&policy),sizeof(policy),0);
    if (status==ERROR_SUCCESS) status=NCryptFinalizeKey(key,0);
    QByteArray name,privateDer,certDer;
    DWORD size=0;
    if (status==ERROR_SUCCESS && CertStrToNameW(X509_ASN_ENCODING,L"CN=LinkDesk Local",CERT_X500_NAME_STR,nullptr,nullptr,&size,nullptr)) {
        name.resize(int(size));
        if (!CertStrToNameW(X509_ASN_ENCODING,L"CN=LinkDesk Local",CERT_X500_NAME_STR,nullptr,reinterpret_cast<BYTE *>(name.data()),&size,nullptr)) status=GetLastError();
    } else if (status==ERROR_SUCCESS) status=GetLastError();
    if (status==ERROR_SUCCESS) {
        CERT_NAME_BLOB subject{DWORD(name.size()),reinterpret_cast<BYTE *>(name.data())};
        CRYPT_ALGORITHM_IDENTIFIER algorithm{}; algorithm.pszObjId=const_cast<char *>(szOID_RSA_SHA256RSA);
        SYSTEMTIME start{},end{}; GetSystemTime(&start); end=start; end.wYear+=10;
        if (end.wMonth==2 && end.wDay==29) end.wDay=28;
        cert=CertCreateSelfSignCertificate(key,&subject,CERT_CREATE_SELFSIGN_NO_KEY_INFO,nullptr,&algorithm,&start,&end,nullptr);
        if (!cert) status=GetLastError();
    }
    if (status==ERROR_SUCCESS) status=NCryptExportKey(key,0,NCRYPT_PKCS8_PRIVATE_KEY_BLOB,nullptr,nullptr,0,&size,0);
    if (status==ERROR_SUCCESS) {
        privateDer.resize(int(size));
        status=NCryptExportKey(key,0,NCRYPT_PKCS8_PRIVATE_KEY_BLOB,nullptr,reinterpret_cast<PBYTE>(privateDer.data()),size,&size,0);
        privateDer.resize(int(size));
    }
    if (cert) { certDer=QByteArray(reinterpret_cast<const char *>(cert->pbCertEncoded),int(cert->cbCertEncoded)); CertFreeCertificateContext(cert); }
    if (key) NCryptFreeObject(key);
    if (provider) NCryptFreeObject(provider);
    const bool ok=status==ERROR_SUCCESS && !privateDer.isEmpty() && !certDer.isEmpty()
        && save(keyPath,pem("PRIVATE KEY",privateDer)) && save(certPath,pem("CERTIFICATE",certDer));
    if (!privateDer.isEmpty()) SecureZeroMemory(privateDer.data(),size_t(privateDer.size()));
    if (!ok) {
        QFile::remove(keyPath); QFile::remove(certPath);
        *error=QString("生成 Windows 本机 TLS 证书失败（系统错误 %1）。").arg(quint32(status));
    }
    return ok;
}
bool Platform::screenCaptureAvailable(QString *error) {
    if (!primaryRect().isEmpty()) return true;
    *error="无法获取当前交互桌面。请登录 Windows 桌面后重试。"; return false;
}
QRect Platform::screenGeometry(QScreen *screen) { Q_UNUSED(screen); return primaryRect(); }
namespace {
struct CaptureSurface {
    HDC memory=nullptr;
    HBITMAP bitmap=nullptr;
    HGDIOBJ previous=nullptr;
    void *pixels=nullptr;
    QSize size;
    ~CaptureSurface() { reset(); }
    void reset() {
        if(memory && previous) SelectObject(memory,previous);
        if(bitmap) DeleteObject(bitmap);
        if(memory) DeleteDC(memory);
        memory=nullptr; bitmap=nullptr; previous=nullptr; pixels=nullptr; size={};
    }
    bool prepare(HDC desktop,const QSize &requested) {
        if(bitmap && size==requested) return true;
        reset(); memory=CreateCompatibleDC(desktop); if(!memory) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=requested.width(); info.bmiHeader.biHeight=-requested.height();
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        bitmap=CreateDIBSection(desktop,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
        if(!bitmap || !pixels) { reset(); return false; }
        previous=SelectObject(memory,bitmap); size=requested; SetStretchBltMode(memory,COLORONCOLOR); return true;
    }
};
}
QImage Platform::captureScreen(QScreen *screen, const QSize &maximum) {
    Q_UNUSED(screen); const auto r=primaryRect(); if (r.isEmpty()) return {};
    const auto accelerated=captureDesktopDXGI(r,maximum.boundedTo(r.size())); if(!accelerated.isNull()) return accelerated;
    const auto size=r.size().boundedTo(maximum);
    const auto captureSize=r.size().scaled(size,Qt::KeepAspectRatio);
    HDC desktop=GetDC(nullptr); if (!desktop) return {};
    // Reuse a directly accessible DIB: avoid creating a device bitmap and then
    // forcing a second GPU-to-CPU readback through GetDIBits on every frame.
    static thread_local CaptureSurface surface;
    QImage image;
    if(surface.prepare(desktop,captureSize)) {
        const bool copied=captureSize==r.size()
            ? BitBlt(surface.memory,0,0,r.width(),r.height(),desktop,r.x(),r.y(),SRCCOPY|CAPTUREBLT)
            : StretchBlt(surface.memory,0,0,captureSize.width(),captureSize.height(),desktop,r.x(),r.y(),r.width(),r.height(),SRCCOPY|CAPTUREBLT);
        if(copied) {
            GdiFlush();
            image=QImage(static_cast<const uchar *>(surface.pixels),captureSize.width(),captureSize.height(),captureSize.width()*4,QImage::Format_RGB32).copy();
        }
    }
    ReleaseDC(nullptr,desktop); return image;
}
QString Platform::permissionHelp() {
    return "Windows：请保持已登录桌面、电脑唤醒。普通权限不能控制管理员窗口、UAC 安全桌面或登录屏幕。";
}

void Platform::initialize() { QSslSocket::setActiveBackend("schannel"); }
QString Platform::bundledConfigPath() { return QCoreApplication::applicationDirPath()+"/server.json"; }

#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDir>
namespace {
QString passwordPath(const QString &account) {
    const auto dir=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/passwords";
    QDir().mkpath(dir);
    return dir+"/"+QString::fromLatin1(QCryptographicHash::hash(account.toUtf8(),QCryptographicHash::Sha256).toHex())+".bin";
}
}
bool Platform::savePassword(const QString &account,const QString &password) {
    auto bytes=password.toUtf8(); DATA_BLOB input{DWORD(bytes.size()),reinterpret_cast<BYTE *>(bytes.data())},output{};
    if (!CryptProtectData(&input,L"LinkDesk",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)) return false;
    const bool result=save(passwordPath(account),QByteArray(reinterpret_cast<const char *>(output.pbData),output.cbData)); LocalFree(output.pbData); return result;
}
QString Platform::loadPassword(const QString &account) {
    QFile file(passwordPath(account)); if (!file.open(QIODevice::ReadOnly)) return {};
    auto bytes=file.readAll(); DATA_BLOB input{DWORD(bytes.size()),reinterpret_cast<BYTE *>(bytes.data())},output{};
    if (!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)) return {};
    const auto result=QString::fromUtf8(reinterpret_cast<const char *>(output.pbData),output.cbData); LocalFree(output.pbData); return result;
}
bool Platform::removePassword(const QString &account) {
    const auto path=passwordPath(account); return !QFile::exists(path) || QFile::remove(path);
}
