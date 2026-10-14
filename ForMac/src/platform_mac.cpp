#include "platform.h"
#include <ApplicationServices/ApplicationServices.h>
#include <QScreen>
#include <QCoreApplication>
#include <QPixmap>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
bool Platform::generateIdentity(const QString &certPath,const QString &keyPath,QString *error) {
        auto openssl = QStandardPaths::findExecutable("openssl");
        if (openssl.isEmpty()) openssl = "/usr/bin/openssl";
        if (openssl.isEmpty()) { *error = "未找到 OpenSSL 命令行工具，请安装后重试（用于首次生成本机 TLS 证书）。"; return false; }
        // Keep the private key owner-only even during creation.
        QFile keyFile(keyPath);
        if (!keyFile.open(QIODevice::WriteOnly)) { *error = "无法创建私钥文件"; return false; }
        keyFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner); keyFile.close();
        QProcess process;
        process.start(openssl,{"req","-x509","-newkey","rsa:2048","-nodes","-sha256",
            "-keyout",keyPath,"-out",certPath,"-days","3650","-subj","/CN=LinkDesk Local"});
        if (!process.waitForFinished(15000) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            process.kill(); process.waitForFinished();
            QFile::remove(keyPath); QFile::remove(certPath);
            *error = "生成本机 TLS 证书失败：" + QString::fromLocal8Bit(process.readAllStandardError()).left(500); return false;
        }
    return true;
}
bool Platform::screenCaptureAvailable(QString *error) {
    if (CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess()) return true;
    *error="请在系统设置 → 隐私与安全性 → 屏幕与系统音频录制中允许 LinkDesk，并退出后重新打开。";
    return false;
}
QRect Platform::screenGeometry(QScreen *screen) { return screen->geometry(); }
QImage Platform::captureScreen(QScreen *screen, const QSize &maximum) {
    auto image=screen->grabWindow(0).toImage();
    if (image.width()>maximum.width() || image.height()>maximum.height())
        image=image.scaled(maximum,Qt::KeepAspectRatio,Qt::SmoothTransformation);
    return image;
}
QString Platform::permissionHelp() { return "macOS：共享需要屏幕录制权限，控制需要辅助功能权限；授权后可能需要重启应用。"; }

void Platform::initialize() {}
QString Platform::bundledConfigPath() { return QCoreApplication::applicationDirPath()+"/../Resources/server.json"; }

#include <CommonCrypto/CommonCryptor.h>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QDir>
namespace {
QString passwordDirectory() { return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/passwords-local"; }
QString passwordPath(const QString &account) { return passwordDirectory()+"/"+QString::fromLatin1(QCryptographicHash::hash(account.toUtf8(),QCryptographicHash::Sha256).toHex())+".bin"; }
QByteArray randomBytes(int count) { QByteArray bytes(count,Qt::Uninitialized); for(auto &byte:bytes) byte=char(QRandomGenerator::system()->generate()&255); return bytes; }
bool writePrivate(const QString &path,const QByteArray &bytes) {
    QSaveFile file(path); if(!file.open(QIODevice::WriteOnly)) return false;
    if(!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner) || file.write(bytes)!=bytes.size()) return false;
    return file.commit();
}
QByteArray localKey(bool create) {
    const auto dir=passwordDirectory(); const auto path=dir+"/local-key.bin"; QFile file(path);
    if(file.exists()) { if(!file.open(QIODevice::ReadOnly) || file.size()!=64) return {}; return file.readAll(); }
    if(!create || !QDir().mkpath(dir) || !QFile::setPermissions(dir,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner)) return {};
    const auto key=randomBytes(64); if(!writePrivate(path,key)) return {}; return key;
}
QByteArray crypt(CCOperation op,const QByteArray &input,const QByteArray &key,const QByteArray &iv) {
    QByteArray output(input.size()+kCCBlockSizeAES128,Qt::Uninitialized); size_t written=0;
    if(CCCrypt(op,kCCAlgorithmAES,kCCOptionPKCS7Padding,key.constData(),32,iv.constData(),input.constData(),input.size(),output.data(),output.size(),&written)!=kCCSuccess) return {};
    output.resize(written); return output;
}
}
bool Platform::savePassword(const QString &account,const QString &password) {
    if(password.size()>256 || password.isEmpty()) return false;
    const auto key=localKey(true); if(key.size()!=64) return false; const auto iv=randomBytes(16); const auto encrypted=crypt(kCCEncrypt,password.toUtf8(),key,iv); if(encrypted.isEmpty()) return false;
    const auto body=QByteArray("LDP1",4)+iv+encrypted;
    // Bind authentication to the account as well as the ciphertext.
    const auto tag=QMessageAuthenticationCode::hash(account.toUtf8()+body,key.mid(32),QCryptographicHash::Sha256);
    return writePrivate(passwordPath(account),body+tag);
}
QString Platform::loadPassword(const QString &account) {
    const auto key=localKey(false); if(key.size()!=64) return {};
    QFile file(passwordPath(account)); if(!file.open(QIODevice::ReadOnly) || file.size()<68 || file.size()>2048) return {};
    const auto bytes=file.readAll(),body=bytes.left(bytes.size()-32),tag=bytes.right(32);
    if(!body.startsWith("LDP1")) return {};
    const auto expected=QMessageAuthenticationCode::hash(account.toUtf8()+body,key.mid(32),QCryptographicHash::Sha256); unsigned int diff=0;
    for(int i=0;i<32;++i) diff|=quint8(tag[i])^quint8(expected[i]); if(diff) return {};
    return QString::fromUtf8(crypt(kCCDecrypt,body.mid(20),key,body.mid(4,16)));
}
bool Platform::removePassword(const QString &account) {
    const auto path=passwordPath(account); return !QFile::exists(path) || QFile::remove(path);
}
