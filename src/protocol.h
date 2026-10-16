#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <functional>
class QSslSocket;

// Wire format: one-byte type + four-byte big-endian length + payload.
class Wire : public QObject {
    Q_OBJECT
public:
    enum Type : quint8 { Json = 1, Frame = 2, Desktop = 3, Video = 4, File = 5 };
    static constexpr quint32 MaxFrame = 8 * 1024 * 1024;
    static constexpr quint32 MaxJson = 16 * 1024;
    explicit Wire(QSslSocket *socket, QObject *parent = nullptr);
    explicit Wire(std::function<bool(Type,const QByteArray &)> sender,QObject *parent=nullptr);
    static QByteArray packet(Type type, const QByteArray &payload);
    bool sendJson(const QJsonObject &json);
    bool sendFrame(const QByteArray &jpeg);
    bool sendDesktop(const QByteArray &packet);
    bool sendFile(const QByteArray &data);
    bool sendVideo(const QByteArray &packet);
    bool visualWritable() const;
    // Public for deterministic fragmentation and abuse tests.
    void feed(const QByteArray &bytes);
signals:
    void jsonReceived(const QJsonObject &json);
    void frameReceived(const QByteArray &jpeg);
    void desktopReceived(const QByteArray &packet);
    void fileReceived(const QByteArray &data);
    void videoReceived(const QByteArray &packet);
    void invalidPacket(const QString &reason);
private:
    QSslSocket *m_socket=nullptr;
    std::function<bool(Type,const QByteArray &)> m_sender;
    QByteArray m_buffer;
    bool m_failed = false;
    bool send(Type type, const QByteArray &payload);
};
