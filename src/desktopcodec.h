#pragma once
#include <QImage>
#include <QByteArray>
#include "captureframe.h"
class DesktopEncoder {
public:
    QByteArray encode(const QImage &image,int quality,bool lossless);
    QByteArray encode(const CaptureFrame &frame,int quality,bool lossless,qint64 nowMs);
    void setOfficeOptimizations(bool enabled) { m_office=enabled; }
    double changedRatio() const { return m_changedRatio; }
    bool needsRefinement() const { return m_office && m_referenceLossy; }
    void commit();
    void reset();
    QByteArray repeatReference() const;
private:
    QImage m_reference,m_candidate;
    quint32 m_sequence=0,m_candidateSequence=0;
    quint64 m_referenceSerial=0,m_candidateSerial=0;
    bool m_office=false,m_referenceLossy=false,m_candidateLossy=false;
    qint64 m_lastChange=0;
    double m_changedRatio=1;
};
class DesktopDecoder {
public:
    bool decode(const QByteArray &packet,QImage *image,bool *changed,QString *error);
    void reset();
private:
    QImage m_image;
    quint32 m_sequence=0;
};
