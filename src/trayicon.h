#pragma once
#include <QIcon>
#include <QPainter>
#include <QPixmap>

namespace Branding {
// Redraw the brand at native tray sizes: no large background tile, no tiny
// chain detail. macOS uses a template mask that adapts to light/dark menu bars.
inline QIcon trayIcon(bool monochrome) {
    QIcon icon;
    for(int size:{16,18,20,22,24,32,36,40,44,48,64}) {
        QPixmap pixmap(size,size);pixmap.fill(Qt::transparent);
        QPainter p(&pixmap);p.setRenderHint(QPainter::Antialiasing);p.scale(size/32.0,size/32.0);
        const QColor ink("#29302d"),sage("#c5dab4"),apricot("#ffd09a");
        auto screen=[&](const QRectF &rect,const QColor &fill) {
            p.setPen(Qt::NoPen);p.setBrush(monochrome ? QColor(Qt::black) : ink);p.drawRoundedRect(rect,2.5,2.5);
            if(monochrome)p.setCompositionMode(QPainter::CompositionMode_Clear);
            else p.setBrush(fill);
            p.drawRoundedRect(rect.adjusted(2.2,2.2,-2.2,-2.2),.7,.7);
            p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        };
        screen(QRectF(1,2,21,14.5),sage);
        p.setPen(QPen(monochrome ? QColor(Qt::black) : ink,2.2,Qt::SolidLine,Qt::RoundCap));
        p.drawLine(QPointF(10,17),QPointF(10,20));p.drawLine(QPointF(6.5,20),QPointF(13.5,20));
        screen(QRectF(11,10.5,20,14.5),apricot);
        p.setPen(QPen(monochrome ? QColor(Qt::black) : ink,2.2,Qt::SolidLine,Qt::RoundCap));
        p.drawLine(QPointF(21,25.5),QPointF(21,29));p.drawLine(QPointF(17,29),QPointF(25,29));
        p.end();icon.addPixmap(pixmap);
    }
    icon.setIsMask(monochrome);return icon;
}
inline QIcon trayIcon() {
#ifdef Q_OS_MACOS
    return trayIcon(true);
#else
    return trayIcon(false);
#endif
}
}
