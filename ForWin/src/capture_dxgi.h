#pragma once
#include <QImage>
#include "captureframe.h"
QImage captureDesktopDXGI(const QRect &geometry,const QSize &maximum);
CaptureFrame captureDesktopFrameDXGI(const QRect &geometry,const QSize &maximum);

void resetDesktopCaptureDXGI();
