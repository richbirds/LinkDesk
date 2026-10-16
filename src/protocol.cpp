#include "protocol.h"
#include <QJsonDocument>
#include <QSslSocket>
#include <QtEndian>

Wire::Wire(QSslSocket *socket, QObject *parent) : QObject(parent), m_socket(socket) {
    if (socket) connect(socket, &QSslSocket::readyRead, this, [this] {
        while (m_socket->bytesAvailable() && !m_failed)
            feed(m_socket->read(64 * 1024));
    });
}
Wire::Wire(std::function<bool(Type,const QByteArray &)> sender,QObject *parent) : QObject(parent),m_sender(std::move(sender)) {}
QByteArray Wire::packet(Type type, const QByteArray &payload) {
    QByteArray result(5, '\0');
    result[0] = char(type);
    qToBigEndian<quint32>(quint32(payload.size()), result.data() + 1);
    result.append(payload);
    return result;
}
bool Wire::send(Type type, const QByteArray &payload) {
    if(payload.isEmpty()) return false;
    if(m_sender) {
        if(payload.size()>(type==Json ? MaxJson : type==File ? 32840 : MaxFrame)) return false;
        return m_sender(type,payload);
    }
    if (!m_socket || !m_socket->isEncrypted()) return false;
    if (payload.size() > (type == Json ? MaxJson : type == File ? 32840 : MaxFrame)) return false;
    // Drop old captures when the connection is slow instead of accumulating frames.
    if (type != Json && m_socket->bytesToWrite() > 256 * 1024) return false;
    if (m_socket->bytesToWrite() > MaxFrame + 65536) {
        m_socket->abort();
        return false;
    }
    auto data = packet(type, payload);
    if(m_socket->write(data)!=data.size()) { m_socket->abort(); return false; }
    return true;
}
bool Wire::sendJson(const QJsonObject &json) { return send(Json, QJsonDocument(json).toJson(QJsonDocument::Compact)); }
bool Wire::sendFrame(const QByteArray &jpeg) { return send(Frame, jpeg); }
bool Wire::sendDesktop(const QByteArray &data) { return send(Desktop,data); }
bool Wire::sendFile(const QByteArray &data) { return data.size()<=32840 && send(File,data); }
bool Wire::sendVideo(const QByteArray &data) { return send(Video,data); }
bool Wire::visualWritable() const { return m_socket && m_socket->isEncrypted() && m_socket->bytesToWrite()<=256*1024; }
void Wire::feed(const QByteArray &bytes) {
    if (m_failed) return;
    auto fail = [this](const QString &reason) {
        m_failed = true;
        m_buffer.clear();
        emit invalidPacket(reason);
        if (m_socket) m_socket->abort();
    };
    if (m_buffer.size() + bytes.size() > MaxFrame + 65536 + 5) {
        fail(QStringLiteral("数据缓冲区超过限制")); return;
    }
    m_buffer.append(bytes);
    while (m_buffer.size() >= 5 && !m_failed) {
        auto type = quint8(m_buffer[0]);
        auto length = qFromBigEndian<quint32>(m_buffer.constData() + 1);
        if ((type != Json && type != Frame && type != Desktop && type != Video && type != File) || length == 0 || length > (type == Json ? MaxJson : type == File ? 32840 : MaxFrame)) {
            fail(QStringLiteral("无效的数据包类型或长度")); return;
        }
        if (m_buffer.size() < qsizetype(length) + 5) return;
        auto payload = m_buffer.mid(5, length);
        m_buffer.remove(0, length + 5);
        if (type == File) emit fileReceived(payload);
        else if (type == Frame) emit frameReceived(payload);
        else if(type == Desktop) emit desktopReceived(payload);
        else if(type == Video) emit videoReceived(payload);
        else {
            QJsonParseError error;
            auto doc = QJsonDocument::fromJson(payload, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                fail(QStringLiteral("无效的 JSON 数据")); return;
            }
            emit jsonReceived(doc.object());
        }
    }
}
