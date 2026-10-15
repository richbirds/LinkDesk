#pragma once
#include <QObject>
#include <QPointer>
#include <QFile>
#include <QTimer>
#include <QQueue>
#include <QJsonObject>
#include <atomic>
#include <memory>
class Wire;
// One bounded outgoing stream per peer; unfinished incoming parts survive reconnects.
class FileTransfer : public QObject {
    Q_OBJECT
public:
    explicit FileTransfer(QObject *parent=nullptr);
    ~FileTransfer() override;
    void shutdown();
    void attach(Wire *wire);
    void cancel();
    void enqueue(const QStringList &paths);
    static QString receiveDirectory();
signals:
    void progress(const QString &name,qint64 done,qint64 total,const QString &state);
    void received(const QString &path);
private:
    std::shared_ptr<std::atomic_bool> m_sendCancel,m_receiveCancel;
    bool m_stopped=false;
    QPointer<Wire> m_wire;
    QQueue<QString> m_queue;
    QFile m_source,m_part;
    QString m_id,m_name,m_hash,m_inId,m_inHash,m_inName,m_directory;
    qint64 m_size=0,m_ack=0,m_sent=0,m_inSize=0;
    quint64 m_generation=0;
    bool m_preparing=false,m_ready=false,m_finishing=false;
    QTimer m_pump,m_retry;
    void next(); void offer(); void pump(); void message(const QJsonObject &j);
    void chunk(const QByteArray &data); void fail(const QString &reason);
    void reply(const QString &type,qint64 offset=0,const QString &error={});
};
