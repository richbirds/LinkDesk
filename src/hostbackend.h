#pragma once
#include "captureframe.h"
#include <QObject>
#include <QJsonObject>
// Optional brokered desktop. The ordinary GUI keeps its existing native path.
class HostBackend : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void setStream(int,const QSize &) {}
    virtual CaptureFrame sample(const QSize &maximum)=0;
    virtual QRect geometry() const=0;
    virtual CursorFrame cursor() const { return {}; }
    virtual void activate(bool active)=0;
    virtual void input(const QJsonObject &event)=0;
    virtual void releaseInput()=0;
    virtual void fileJson(const QJsonObject &message)=0;
    virtual void fileData(const QByteArray &data)=0;
    virtual bool filesAllowed() const=0;
    virtual bool power(const QString &action,QString *error)=0;
    virtual bool sasAvailable() const=0;
    virtual bool sas(QString *error)=0;
signals:
    void desktopBoundary();
    void fileReply(const QJsonObject &message);
    void fileChunk(const QByteArray &data);
};
