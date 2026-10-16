#include "viewer.h"
#include "remote.h"
#include <QBuffer>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QSpinBox>
#include <QComboBox>
#include <QSettings>
#include <QVBoxLayout>
#include <QFocusEvent>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QToolBar>
#include <QWheelEvent>

static int baseKey(int k) {
    switch (k) {
    case Qt::Key_Exclam: return Qt::Key_1; case Qt::Key_At: return Qt::Key_2;
    case Qt::Key_NumberSign: return Qt::Key_3; case Qt::Key_Dollar: return Qt::Key_4;
    case Qt::Key_Percent: return Qt::Key_5; case Qt::Key_AsciiCircum: return Qt::Key_6;
    case Qt::Key_Ampersand: return Qt::Key_7; case Qt::Key_Asterisk: return Qt::Key_8;
    case Qt::Key_ParenLeft: return Qt::Key_9; case Qt::Key_ParenRight: return Qt::Key_0;
    case Qt::Key_Underscore: return Qt::Key_Minus; case Qt::Key_Plus: return Qt::Key_Equal;
    case Qt::Key_BraceLeft: return Qt::Key_BracketLeft; case Qt::Key_BraceRight: return Qt::Key_BracketRight;
    case Qt::Key_Bar: return Qt::Key_Backslash; case Qt::Key_Colon: return Qt::Key_Semicolon;
    case Qt::Key_QuoteDbl: return Qt::Key_Apostrophe; case Qt::Key_Less: return Qt::Key_Comma;
    case Qt::Key_Greater: return Qt::Key_Period; case Qt::Key_Question: return Qt::Key_Slash;
    case Qt::Key_AsciiTilde: return Qt::Key_QuoteLeft; default: return k;
    }
}
RemoteCanvas::RemoteCanvas(QWidget *parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus); setMouseTracking(true); setMinimumSize(480,270); m_move.start();
}
QRectF RemoteCanvas::imageRect() const {
    if (m_image.isNull()) return {};
    auto s = m_image.size().scaled(size(),Qt::KeepAspectRatio);
    return QRectF((width()-s.width())/2.0,(height()-s.height())/2.0,s.width(),s.height());
}
void RemoteCanvas::setFrame(const QByteArray &jpeg) {
    QBuffer buffer; buffer.setData(jpeg); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const auto s = reader.size();
    // Check advertised dimensions before allocating an untrusted compressed image.
    if (!s.isValid() || s.width() > 4096 || s.height() > 4096 || qint64(s.width())*s.height() > 8000000) {
        emit invalidFrame(); return;
    }
    auto image = reader.read(); if (image.isNull()) { emit invalidFrame(); return; }
    m_image=std::move(image); setProperty("encodedFrameSize",m_image.size()); update();
}
void RemoteCanvas::setImage(const QImage &image) { m_image=image; setProperty("encodedFrameSize",image.size()); update(); }
void RemoteCanvas::setControl(bool enabled) { releaseInput(); m_control=enabled; update(); }
void RemoteCanvas::clearFrame() { releaseInput(); m_image=QImage();m_cursorVisible=false; setProperty("encodedFrameSize",QSize()); update(); }
void RemoteCanvas::setRemoteCursor(const QImage &image,const QPoint &hotspot,const QPointF &position,bool visible) { m_cursorImage=image;m_cursorHotspot=hotspot;m_cursorPosition=position;m_cursorVisible=visible;update(); }
void RemoteCanvas::releaseInput() {
    if (m_control) emit input({{"action","release"}});
    m_keys.clear();
}
void RemoteCanvas::paintEvent(QPaintEvent *) {
    QPainter p(this); p.fillRect(rect(),QColor("#0c1220"));
    if (m_image.isNull()) { p.setPen(QColor("#a5b1c5")); p.drawText(rect(),Qt::AlignCenter,"正在等待远程画面…"); }
    else { p.setRenderHint(QPainter::SmoothPixmapTransform); const auto bounds=imageRect();p.drawImage(bounds,m_image);
        if(!m_control && m_cursorVisible && !m_cursorImage.isNull()) { p.save();p.setClipRect(bounds);const double scale=bounds.width()/m_image.width(); const QPointF position(bounds.x()+m_cursorPosition.x()*bounds.width()-m_cursorHotspot.x()*scale,bounds.y()+m_cursorPosition.y()*bounds.height()-m_cursorHotspot.y()*scale);p.drawImage(QRectF(position,QSizeF(m_cursorImage.size())*scale),m_cursorImage);p.restore(); }
    }
}
void RemoteCanvas::pointer(QMouseEvent *e, bool isButton, bool down) {
    if (!m_control || m_image.isNull()) return;
    const auto r = imageRect();
    if (!r.contains(e->position()) && !(isButton && !down)) return;
    QJsonObject j{{"action",isButton ? "button" : "move"},
        {"x",qBound(0.0,(e->position().x()-r.x())/r.width(),1.0)},
        {"y",qBound(0.0,(e->position().y()-r.y())/r.height(),1.0)}};
    if (isButton) {
        int b=e->button() == Qt::LeftButton ? 1 : e->button() == Qt::RightButton ? 2 : e->button() == Qt::MiddleButton ? 3 : 0;
        if (!b) return; j["button"]=b; j["down"]=down;
    }
    emit input(j);
}
void RemoteCanvas::mouseMoveEvent(QMouseEvent *e) {
    if (m_move.elapsed() < 16) return; m_move.restart(); pointer(e,false,false);
}
void RemoteCanvas::mousePressEvent(QMouseEvent *e) { setFocus(); pointer(e,true,true); }
void RemoteCanvas::mouseReleaseEvent(QMouseEvent *e) { pointer(e,true,false); }
void RemoteCanvas::wheelEvent(QWheelEvent *e) {
    if (m_control && imageRect().contains(e->position())) {
        int ticks=e->angleDelta().y()/120;
        if (!ticks && e->pixelDelta().y()) ticks = e->pixelDelta().y() > 0 ? 1 : -1;
        emit input({{"action","scroll"},{"ticks",qBound(-10,ticks,10)}});
    }
    e->accept();
}
void RemoteCanvas::keyPressEvent(QKeyEvent *e) {
    if (e->key() == Qt::Key_Escape && e->modifiers().testFlag(Qt::ControlModifier) && e->modifiers().testFlag(Qt::ShiftModifier)) {
        releaseInput(); emit escapeRequested(); return;
    }
    if (!m_control || e->isAutoRepeat()) return;
    if(e->key()==Qt::Key_Delete && e->modifiers().testFlag(Qt::ControlModifier) && e->modifiers().testFlag(Qt::AltModifier)){releaseInput();emit secureAttentionRequested();e->accept();return;}
    int k=baseKey(e->key()); if (m_keys.contains(k)) return;
    m_keys.insert(k); emit input({{"action","key"},{"key",k},{"down",true}}); e->accept();
}
void RemoteCanvas::keyReleaseEvent(QKeyEvent *e) {
    if (!m_control || e->isAutoRepeat()) return;
    int k=baseKey(e->key()); if (m_keys.remove(k)) emit input({{"action","key"},{"key",k},{"down",false}});
    e->accept();
}
void RemoteCanvas::focusOutEvent(QFocusEvent *e) { releaseInput(); QWidget::focusOutEvent(e); }
ViewerWindow::ViewerWindow(RemoteClient *client, QWidget *parent) : QMainWindow(parent),m_client(client) {
    setWindowFlag(Qt::Window,true); resize(1100,740);
    m_canvas=new RemoteCanvas; setCentralWidget(m_canvas);
    auto bar=addToolBar("会话"); bar->setMovable(false); bar->setFloatable(false);
    bar->setStyleSheet("QToolBar{background:#17233b;border:0;padding:10px;spacing:14px;} QLabel{color:#dce6f7;} QPushButton{color:#f2f6ff;background:#2a3c5d;border:1px solid #64799a;border-radius:8px;padding:7px 16px;} QPushButton:hover{background:#39527b;} QPushButton:pressed{background:#20314e;}");
    m_mode=new QLabel; bar->addWidget(m_mode);
    auto stretch=new QWidget; stretch->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred); bar->addWidget(stretch);
    m_stats=new QLabel("等待画面"); m_stats->setObjectName("liveSessionStats"); bar->addWidget(m_stats);
    QSettings stored;
    client->setStreamSettings(StreamSettings::fromJson(stored.value("viewer/streamSettings").toJsonObject()));
    auto options=new QPushButton("画质 / 帧率"); options->setObjectName("streamSettingsButton"); bar->addWidget(options);
    connect(options,&QPushButton::clicked,this,[this] {
        m_canvas->releaseInput();
        QDialog dialog(this); dialog.setWindowTitle("画质与目标帧率");
        auto layout=new QVBoxLayout(&dialog); auto form=new QFormLayout; layout->addLayout(form);
        const auto current=m_client->streamSettings();
        QSpinBox fps,quality; fps.setRange(1,30); fps.setValue(current.fps); fps.setSuffix(" FPS");
        quality.setRange(40,95); quality.setValue(current.quality);
        QComboBox resolution,format,codec;
        resolution.addItem("1280 × 720",1280); resolution.addItem("1920 × 1080",1920); resolution.addItem("2560 × 1440",2560);
        resolution.setCurrentIndex(resolution.findData(current.width));
        format.addItem("自动优化 · 静态文字无损修复"); format.addItem("全部区域无损"); format.setCurrentIndex(current.lossless ? 1 : 0);
        codec.addItem("自动选择",QString("auto"));codec.addItem("增量图像",QString("desktop"));codec.addItem("优先硬件 H.264",QString("h264"));codec.setCurrentIndex(codec.findData(current.codec));
        quality.setEnabled(!current.lossless);
        connect(&format,&QComboBox::currentIndexChanged,&quality,[&](int index){ quality.setEnabled(index==0); });
        form->addRow("目标帧率",&fps); form->addRow("最高分辨率",&resolution); form->addRow("压缩方式",&format);
        form->addRow("JPEG 画质",&quality);
        form->addRow("动态画面编码",&codec);
        auto help=new QLabel("静止时复用画面，稳定后自动修复文字清晰度。\n动态画面可使用硬件编码；不可用时自动回退。\n网络拥堵时控制排队，不自动缩小画面。无损模式使用增量图像。"); help->setWordWrap(true); layout->addWidget(help);
        QDialogButtonBox buttons(QDialogButtonBox::Ok|QDialogButtonBox::Cancel); layout->addWidget(&buttons);
        connect(&buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept); connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
        if (dialog.exec()==QDialog::Accepted) {
            auto next=current; next.fps=fps.value(); next.quality=quality.value(); next.lossless=format.currentIndex()==1;
            next.codec=codec.currentData().toString();
            next.width=resolution.currentData().toInt(); next=StreamSettings::fromJson(next.json());
            m_fullSettings=next; m_client->setStreamSettings(next); QSettings().setValue("viewer/streamSettings",next.json());
        }
        m_canvas->setFocus();
    });
    connect(client,&RemoteClient::streamSettingsApplied,this,[this](const QJsonObject &j){
        const auto s=StreamSettings::fromJson(j);
        m_stats->setToolTip(QString("目标 %1 FPS · 分辨率上限 %2×%3 · %4 · 不限带宽")
            .arg(s.fps).arg(s.width).arg(s.height).arg(s.lossless ? "PNG 无损" : QString("JPEG %1").arg(s.quality)));
    });
    auto sas=new QPushButton("Ctrl+Alt+Del");sas->setObjectName("serviceSasButton");bar->addWidget(sas);
    connect(sas,&QPushButton::clicked,client,&RemoteClient::requestSecureAttention);
    connect(client,&RemoteClient::connected,sas,[sas,client]{sas->setEnabled(client->property("serviceSas").toBool());});
    sas->setEnabled(client->active() && client->property("serviceSas").toBool());
    connect(client,&RemoteClient::desktopReset,this,[this]{m_canvas->clearFrame();m_canvas->setRemoteCursor({}, {}, {}, false);m_stats->setText("正在切换桌面…");});
    auto fullscreen=new QPushButton("全屏"); bar->addWidget(fullscreen);
    connect(fullscreen,&QPushButton::clicked,this,[this,fullscreen] {
        if (isFullScreen()) { showNormal(); fullscreen->setText("全屏"); }
        else { showFullScreen(); fullscreen->setText("退出全屏"); }
        m_canvas->setControl(m_controlAllowed && !m_preview);
        m_canvas->setFocus();
    });
    m_leave=new QPushButton("断开连接"); bar->addWidget(m_leave); connect(m_leave,&QPushButton::clicked,this,&QWidget::close);
    connect(m_canvas,&RemoteCanvas::input,client,&RemoteClient::sendInput);
    connect(m_canvas,&RemoteCanvas::secureAttentionRequested,client,&RemoteClient::requestSecureAttention);
    connect(m_canvas,&RemoteCanvas::escapeRequested,this,[this] { if (isFullScreen()) showNormal(); else close(); });
    connect(m_canvas,&RemoteCanvas::invalidFrame,client,&RemoteClient::disconnectPeer);
    connect(client,&RemoteClient::imageFrame,this,[this](const QImage &image,qint64 bytes,bool changed) {
        if(m_managed && m_preview) return;
        if(changed || !m_canvas->property("encodedFrameSize").toSize().isValid()) m_canvas->setImage(image);
        if(changed) ++m_frames; m_bytes+=bytes;
    });
    connect(client,&RemoteClient::cursorFrame,m_canvas,&RemoteCanvas::setRemoteCursor);
    connect(client,&RemoteClient::streamMetrics,this,[this](const QJsonObject &j) { m_stats->setToolTip(QString("编码 %1 ms · ACK 往返 %2 ms · 采集排队 %3 ms · 在途 %4 帧").arg(j.value("encodeMs").toDouble(),0,'f',1).arg(j.value("ackRttMs").toDouble(),0,'f',1).arg(j.value("captureQueueMs").toDouble(),0,'f',1).arg(j.value("pendingFrames").toInt())); });
    m_statsTimer.setInterval(1000);
    connect(&m_statsTimer,&QTimer::timeout,this,[this] {
        const auto elapsed=qMax<qint64>(1,m_clock.restart());
        const auto size=m_canvas->property("encodedFrameSize").toSize();
        m_stats->setText(m_frames==0 && size.isValid()
            ? QString("静止画面 · %1 KB/s · %2×%3").arg(double(m_bytes)/elapsed,0,'f',1).arg(size.width()).arg(size.height())
            : QString("%1 FPS  ·  %2 KB/s  ·  %3×%4").arg(double(m_frames)*1000.0/elapsed,0,'f',1).arg(double(m_bytes)/elapsed,0,'f',1).arg(size.width()).arg(size.height()));
        m_frames=0; m_bytes=0;
    });
    connect(client,&RemoteClient::controlChanged,this,[this](bool control) {
        m_controlAllowed=control; m_canvas->setControl(control && !m_preview);
        if(!m_preview) m_mode->setText(control ? "●  可控制 · Ctrl+Shift+Esc 退出" : "●  仅观看 · 请在被控端勾选「允许鼠标键盘控制」");
    });
    connect(client,&RemoteClient::disconnected,this,[this] { m_canvas->setControl(false); m_statsTimer.stop(); hide(); });
}
void ViewerWindow::begin(const QString &name, bool control) {
    m_controlAllowed=control;
    if (m_managed) {
        m_fullSettings=StreamSettings::fromJson(QSettings().value("viewer/streamSettings").toJsonObject());
        m_preview=false; m_client->setStreamSettings(m_fullSettings); emit expanded();
    }
    m_canvas->clearFrame();
    m_canvas->setControl(control); m_clock.start(); m_frames=0; m_bytes=0; m_stats->setText("等待画面"); m_statsTimer.start();
    setWindowTitle(name + " · LinkDesk 远程桌面");
    m_mode->setText(control ? "●  可控制 · Ctrl+Shift+Esc 退出" : "●  仅观看 · 请在被控端勾选「允许鼠标键盘控制」");
    if(isMinimized()) showNormal(); else show();
    raise(); activateWindow(); m_canvas->setFocus();
}
void ViewerWindow::preview() {
    m_preview=true; m_canvas->releaseInput(); m_canvas->setControl(false); m_statsTimer.stop(); hide();
    m_mode->setText("●  缩略图模式 · 输入已暂停");
    StreamSettings settings; settings.width=480; settings.height=270; settings.fps=1; settings.quality=20;
    m_client->setStreamSettings(settings); emit returnedToPreview();
}
void ViewerWindow::setManaged(bool managed) { m_managed=managed; m_leave->setText(managed ? "返回缩略图" : "断开连接"); }
void ViewerWindow::changeEvent(QEvent *e) {
    QMainWindow::changeEvent(e);
    if (m_managed && e->type()==QEvent::WindowStateChange && isMinimized()) preview();
}
void ViewerWindow::closeEvent(QCloseEvent *e) {
    if (m_managed) { preview(); e->accept(); return; }
    m_canvas->releaseInput(); m_statsTimer.stop(); m_client->disconnectPeer(); e->accept();
}
