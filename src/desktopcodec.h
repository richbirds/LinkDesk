#pragma once
#include <QImage>
#include <QByteArray>
class DesktopEncoder {
public:
    QByteArray encode(const QImage &image,int quality,bool lossless);
    void commit();
    void reset();
private:
    QImage m_reference,m_candidate;
    quint32 m_sequence=0,m_candidateSequence=0;
};
class DesktopDecoder {
public:
    bool decode(const QByteArray &packet,QImage *image,bool *changed,QString *error);
    void reset();
private:
    QImage m_image;
    quint32 m_sequence=0;
};
