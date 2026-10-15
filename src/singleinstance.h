#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <functional>

// Scope ownership to the data directory, so copies launched from different
// installation/build folders cannot concurrently use the same saved identity.
class SingleInstance {
public:
    enum Result { Owner, AlreadyRunning, Failed };
    explicit SingleInstance(const QString &directory)
        : m_directory(QDir(directory).absolutePath()),m_lock(m_directory+"/instance.lock"),
          m_name("LinkDesk-"+QString::fromLatin1(QCryptographicHash::hash(m_directory.toUtf8(),QCryptographicHash::Sha256).toHex().left(24))) {
        m_lock.setStaleLockTime(0);
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
    }
    Result acquire(std::function<void()> activate,QString *error=nullptr) {
        if(!QDir().mkpath(m_directory)) {
            if(error) *error="无法创建本机数据目录。";
            return Failed;
        }
        if(!m_lock.tryLock(0)) {
            if(m_lock.error()!=QLockFile::LockFailedError) {
                if(error) *error="无法锁定本机设备身份，请检查数据目录权限。";
                return Failed;
            }
            QLocalSocket existing;existing.connectToServer(m_name);
            if(existing.waitForConnected(1000)) existing.disconnectFromServer();
            return AlreadyRunning;
        }
        // Only the lock owner may remove a socket left behind by a crash.
        QLocalServer::removeServer(m_name);
        if(!m_server.listen(m_name)) {
            if(error) *error="无法创建本机窗口唤醒通道："+m_server.errorString();
            m_lock.unlock();return Failed;
        }
        QObject::connect(&m_server,&QLocalServer::newConnection,&m_server,[this,activate] {
            while(auto connection=m_server.nextPendingConnection()) {
                connection->disconnectFromServer();connection->deleteLater();
                activate();
            }
        });
        return Owner;
    }
private:
    QString m_directory;
    QLockFile m_lock;
    QString m_name;
    QLocalServer m_server;
};
