#pragma once
#include <QImage>
#include <QPainter>
// All scenes are generated locally; no real desktop, user files or screenshots.
inline QImage syntheticDesktop(const QString &scene,int frame,QSize size=QSize(1280,720)) {
    QImage image(size,QImage::Format_RGB32);image.fill(QColor("#faf7f1"));
    QPainter p(&image);p.setFont(QFont("Arial",16));
    if(scene=="video") {
        for(int y=0;y<size.height();y+=8) for(int x=0;x<size.width();x+=8) p.fillRect(x,y,8,8,QColor::fromHsv((x+y+frame*7)%360,190,(x*3+y+frame*5)%180+70));
        p.setBrush(Qt::white);p.drawEllipse(QPoint((frame*17)%size.width(),size.height()/2),80,80);
    } else {
        const int offset=scene=="scroll" ? frame*8 : 0;
        p.save();p.translate(0,-offset);
        for(int line=0;line<120;++line) { p.setPen(QColor((line*19)%180,(line*11)%140,(line*7)%160));p.drawText(30,35+line*28,QString("LinkDesk synthetic document %1 : clear text, numbers 0123456789").arg(line)); }
        p.restore();
        if(scene=="typing") { p.fillRect(25,60,1000,42,Qt::white);p.setPen(Qt::black);p.drawText(30,90,QString("Typing: ")+QString(qMin(frame,80),'A')); }
        if(scene=="drag") { const int x=80+(frame*9)%650,y=120+(frame*3)%250;p.fillRect(x,y,380,240,QColor("#ead8ba"));p.fillRect(x,y,380,28,QColor("#758d73"));p.setPen(Qt::black);p.drawText(x+20,y+80,"Synthetic window"); }
    }
    return image;
}
