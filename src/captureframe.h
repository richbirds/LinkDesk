#pragma once
#include <QImage>
#include <QRegion>
#include <QList>
#include <QPointF>
struct DesktopMove { QPoint source; QRect destination; };
struct CaptureFrame {
    QImage image;
    QRegion dirty;
    QList<DesktopMove> moves;
    quint64 serial=0, previousSerial=0;
    bool regionsValid=false, unchanged=false, pending=false;
};
struct CursorFrame {
    QImage image;
    QPoint hotspot;
    QPointF position;
    bool visible=false;
};
