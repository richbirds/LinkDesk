#include "connectionlist.h"
#include "relay.h"
#include "remote.h"
#include "viewer.h"
#include "platform.h"
#include <QBuffer>
#include <QMenu>
#include <QToolButton>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QProgressBar>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSslSocket>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

static StreamSettings thumbnailSettings() {
    StreamSettings s; s.width=480; s.height=270; s.fps=1; s.quality=20; return s;
}
static bool localSubnet(const QString &host) {
    QHostAddress peer(host);
    if (peer.protocol()!=QAbstractSocket::IPv4Protocol || peer.isLoopback()) return false;
    for (const auto &interface:QNetworkInterface::allInterfaces()) for (const auto &entry:interface.addressEntries())
        if (entry.ip().protocol()==QAbstractSocket::IPv4Protocol && !entry.ip().isLoopback() && entry.prefixLength()>=8 && peer.isInSubnet(entry.ip(),entry.prefixLength())) return true;
    return false;
}
class Preview : public QWidget {
    Q_OBJECT
public:
    QImage image;
    int state=0;
    QToolButton *actions;
    explicit Preview(QWidget *parent=nullptr):QWidget(parent) { setContextMenuPolicy(Qt::PreventContextMenu);setMinimumHeight(120); setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed); setToolTip("双击绿色缩略图，打开远程桌面；右键显示常用操作");
        actions=new QToolButton(this);actions->setText("⋯");actions->setToolTip("常用操作");actions->setFixedSize(28,22);actions->setStyleSheet("QToolButton{background:#edf2fa;color:#385274;border:0;border-radius:5px;font-size:18px;}");
        connect(actions,&QToolButton::clicked,this,[this]{emit menuRequested(actions->mapToGlobal(QPoint(0,actions->height())));});
    }
    bool frame(const QByteArray &bytes) {
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer); reader.setDecideFormatFromContent(true);
        const auto size=reader.size();
        if (!size.isValid() || size.width()>4096 || size.height()>4096 || qint64(size.width())*size.height()>8000000) return false;
        reader.setScaledSize(size.scaled(480,270,Qt::KeepAspectRatio)); auto decoded=reader.read(); if(decoded.isNull()) return false;
        image=decoded; update(); return true;
    }
signals:
    void opened();
    void menuRequested(const QPoint &globalPosition);
protected:
    void resizeEvent(QResizeEvent *e) override {QWidget::resizeEvent(e);actions->move(width()-62,6);}
    void mouseReleaseEvent(QMouseEvent *e) override {if(e->button()==Qt::RightButton) emit menuRequested(e->globalPosition().toPoint());else QWidget::mouseReleaseEvent(e);}
    void mouseDoubleClickEvent(QMouseEvent *e) override { if (e->button()==Qt::LeftButton) emit opened(); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.fillRect(rect(),QColor("#101b32"));
        if (!image.isNull()) { QSize size=image.size().scaled(this->size(),Qt::KeepAspectRatio); p.drawImage(QRect(QPoint((width()-size.width())/2,(height()-size.height())/2),size),image); }
        else { p.setPen(QColor("#c8d4e8")); p.drawText(rect(),Qt::AlignCenter,"暂无画面"); }
        p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(Qt::white,2));
        p.setBrush(QColor(state==2 ? "#22b77a" : state==1 ? "#e65252" : "#8b95a5")); p.drawEllipse(QPoint(width()-17,17),7,7);
    }
};
class DeviceCard : public QFrame {
    Q_OBJECT
public:
    QString uid,target,name,group,password,endpoint;
    bool remember=false, automatic=true, wanted=false, busy=false, control=false, expanded=false, openWhenReady=false;
    int generation=0;
    qint64 lastFrame=0;
    QElapsedTimer previewClock;
    RelayAgent *agent;
    RemoteClient client;
    ViewerWindow *viewer;
    QPointer<QObject> channel;
    Preview *preview;
    QLabel *caption,*status;
    QPushButton *toggle;
    QTimer retry,watch;
    QProgressBar *transferBar;
    QLabel *transferText;
    QTimer transferRateTimer;
    QElapsedTimer transferRateClock;
    QString transferName,transferState;
    qint64 transferDone=0,transferTotal=0,transferSample=0;
    double transferRate=0;
    void updateTransferText() {
        QString speed;
        if(transferState=="传送中" || transferState=="接收中") {
            speed=" · "+(transferRate>=1024*1024 ? QString::number(transferRate/(1024*1024),'f',2)+" MB/s" : transferRate>=1024 ? QString::number(transferRate/1024,'f',1)+" KB/s" : QString::number(qMax(0.0,transferRate),'f',0)+" B/s");
        }
        transferText->setText(transferName+" · "+transferState+speed);
    }
    DeviceCard(RelayAgent *relay,QWidget *parent):QFrame(parent),agent(relay),client(this) {
        setAcceptDrops(true);setObjectName("card"); setMinimumWidth(245); setMaximumWidth(600);
        auto layout=new QVBoxLayout(this); layout->setContentsMargins(10,10,10,10);layout->setSpacing(6);
        preview=new Preview; layout->addWidget(preview);
        connect(preview,&Preview::menuRequested,this,[this](const QPoint &position){showMenu(position);});
        caption=new QLabel; caption->setTextFormat(Qt::PlainText); caption->setWordWrap(true); caption->setStyleSheet("font-weight:600;font-size:16px;"); layout->addWidget(caption);
        status=new QLabel("未连接"); status->setTextFormat(Qt::PlainText); status->setWordWrap(true); status->setMinimumHeight(22); layout->addWidget(status);
        auto row=new QHBoxLayout; toggle=new QPushButton("连接"); auto modify=new QPushButton("修改"),remove=new QPushButton("删除"); row->addWidget(toggle); row->addWidget(modify); row->addWidget(remove); layout->addLayout(row);
        transferText=new QLabel;transferText->setObjectName("fileTransferStatus");transferText->setTextFormat(Qt::PlainText);transferText->setWordWrap(true);transferText->hide();layout->addWidget(transferText);
        transferBar=new QProgressBar;transferBar->setRange(0,1000);transferBar->setFixedHeight(14);transferBar->hide();layout->addWidget(transferBar);
        connect(&client,&RemoteClient::fileProgress,this,[this](const QString &name,qint64 done,qint64 total,const QString &state){
            const bool streaming=state=="传送中" || state=="接收中";
            if(name!=transferName || (streaming && state!=transferState) || done<transferDone) {
                transferRateClock.restart();transferSample=done;transferRate=0;
            }
            transferName=name;transferState=state;transferDone=done;transferTotal=total;
            if(streaming) {if(!transferRateTimer.isActive())transferRateTimer.start();}else transferRateTimer.stop();
            updateTransferText();transferText->show();transferBar->show();transferBar->setValue(total ? int(double(done)/total*1000) : 0);
            transferBar->setFormat(total ? QString::number(int(double(done)/total*100))+"%" : state);
        });
        transferRateTimer.setInterval(1000);
        connect(&transferRateTimer,&QTimer::timeout,this,[this]{
            const auto elapsed=transferRateClock.elapsed();if(elapsed<=0)return;
            transferRate=qMax<qint64>(0,transferDone-transferSample)*1000.0/elapsed;transferSample=transferDone;transferRateClock.restart();updateTransferText();
        });
        connect(&client,&RemoteClient::powerResult,this,[this](const QString &message){QMessageBox::information(this,"远程电源操作",message);});
        viewer=new ViewerWindow(&client,this); viewer->setManaged(true); viewer->preview();
        connect(modify,&QPushButton::clicked,this,[this]{emit editRequested();});
        connect(remove,&QPushButton::clicked,this,[this]{emit removeRequested();});
        connect(toggle,&QPushButton::clicked,this,[this]{
            if(wanted) { stop(); return; }
            if(password.isEmpty() && remember) password=Platform::loadPassword(account());
            if(password.isEmpty()) { bool ok=false; password=QInputDialog::getText(this,"连接密码",name+" 的连接密码：",QLineEdit::Password,{},&ok); if(!ok) return; }
            if(password.size()<8 || password.size()>64) { setState(1,"请输入 8–64 字符的连接密码"); return; }
            wanted=true; start();
        });
        connect(preview,&Preview::opened,this,[this]{
            if(!client.active() || preview->state!=2) return;
            emit foregroundRequested();
            if(expanded && viewer->isVisible() && !viewer->isMinimized()) { viewer->raise(); viewer->activateWindow(); return; }
            viewer->begin(name,control);
        });
        connect(viewer,&ViewerWindow::expanded,this,[this]{ expanded=true; emit foregroundRequested(); });
        connect(viewer,&ViewerWindow::returnedToPreview,this,[this]{expanded=false;});
        connect(&client,&RemoteClient::controlChanged,this,[this](bool enabled){control=enabled;});
        connect(&client,&RemoteClient::connected,this,[this](const QString &,bool enabled){
            busy=false; control=enabled; lastFrame=QDateTime::currentMSecsSinceEpoch(); client.setStreamSettings(thumbnailSettings()); setState(0,"已认证，等待画面 · "+endpoint);
        });
        connect(&client,&RemoteClient::imageFrame,this,[this](const QImage &image,qint64,bool changed){
            if((changed || preview->image.isNull()) && (!expanded || !previewClock.isValid() || previewClock.elapsed()>=1000)) {
                preview->image=image.scaled(480,270,Qt::KeepAspectRatio,Qt::SmoothTransformation); preview->update(); previewClock.start();
            }
            lastFrame=QDateTime::currentMSecsSinceEpoch(); setState(2,(expanded ? "远程桌面" : "缩略图 · 1 FPS / JPEG 20")+QString(" · ")+endpoint);
            if(openWhenReady) { openWhenReady=false; viewer->begin(name,control); }
        });
        connect(&client,&RemoteClient::streamMetrics,this,[this](const QJsonObject &metrics){
            if(qEnvironmentVariableIsSet("LINKDESK_STREAM_TRACE")) qInfo().noquote()<<"Viewer metrics"<<QJsonDocument(metrics).toJson(QJsonDocument::Compact);
            // v0.7.0 hosts suppress unchanged image packets. A live capture loop
            // with no unacknowledged pictures confirms the last image is current.
            if(client.active() && !preview->image.isNull() && metrics.contains("pendingFrames") && metrics.value("pendingFrames").toInt(-1)==0) {
                lastFrame=QDateTime::currentMSecsSinceEpoch();
                setState(2,(expanded ? "远程桌面" : "缩略图 · 画面静止 / 低带宽")+QString(" · ")+endpoint);
            }
        });
        connect(&client,&RemoteClient::error,this,[this](const QString &reason){failure(reason);});
        connect(&client,&RemoteClient::disconnected,this,[this]{
            if(!busy && !channel && !wanted) return;
            cleanup(); if(wanted) { setState(1,"连接中断，稍后自动重试"); retry.start(30000); }
        });
        connect(&client,&RemoteClient::certificateCheck,this,[this](const QString &,const QString &){ client.trustCertificate(false); failure("缺少可信设备指纹，连接已拒绝"); });
        retry.setSingleShot(true); connect(&retry,&QTimer::timeout,this,[this]{start();});
        watch.setInterval(3000); watch.start(); connect(&watch,&QTimer::timeout,this,[this]{
            if(client.active() && lastFrame && QDateTime::currentMSecsSinceEpoch()-lastFrame>15000) setState(1,"超过 15 秒未收到画面");
        });
    }
    ~DeviceCard() override { stop(); }
    QString account() const { const auto c=agent->config(); return "list:"+uid+":"+c.host.toLower()+":"+QString::number(c.port); }
    void updateCaption() { caption->setText(name==target || name=="设备 "+target ? target : name+" · "+target); setProperty("deviceId",target); preview->setObjectName("preview-"+target); }
    void setState(int state,const QString &text) { preview->state=state; preview->update(); status->setText(text); status->setToolTip(text); toggle->setText(wanted ? "断开" : "连接"); emit stateChanged(text,state,wanted); }
    void cleanup() {
        transferRateTimer.stop();transferRate=0;if(transferState=="传送中") {transferState="连接中断，等待续传";updateTransferText();}
        ++generation; busy=false; auto old=channel; channel.clear();
        if(old) { if(auto tunnel=old->findChild<RelayTunnel *>()) tunnel->close(); old->deleteLater(); }
        client.disconnectPeer(); viewer->hide(); expanded=false;
    }
    void stop() { wanted=false; openWhenReady=false; retry.stop(); cleanup(); preview->image=QImage(); setState(0,"未连接"); }
    void failure(const QString &reason) {
        cleanup(); const bool offline=reason.contains("不在线") || reason.contains("未开启共享");
        if(reason.contains("验证码错误") || reason.contains("密码错误")) { wanted=false; retry.stop(); }
        setState((offline || reason.contains("离线") || reason.contains("尚未开启共享")) ? 0 : 1,reason); if(wanted) retry.start(60000);
    }
    void suspended() { if(!wanted) return; retry.stop(); cleanup(); setState(0,"等待服务器重新连接"); }
    void start() {
        retry.stop(); if(!wanted || busy || client.active()) return;
        if(!agent->online()) { setState(0,"等待服务器上线"); return; }
        if(target==agent->deviceId()) { wanted=false; setState(1,"不能连接本机设备 ID"); return; }
        if(password.isEmpty() && remember) password=Platform::loadPassword(account());
        if(password.isEmpty()) { wanted=false; setState(1,"缺少密码，请点击连接输入"); return; }
        busy=true; auto owner=new QObject(this); channel=owner; const int gen=++generation;
        client.setStreamSettings(thumbnailSettings()); setState(0,"正在连接…");
        auto relay=[this,owner,gen] {
            if(generation!=gen || channel!=owner) return;
            ++generation; // Ignore late resolve/probe callbacks after selecting a transport.
            auto tunnel=agent->createViewerTunnel(target,owner);
            connect(tunnel,&RelayTunnel::viewerReady,owner,[this,owner](quint16 port,const QString &fp){ if(channel!=owner) return; endpoint="公网中转"; client.connectTo("127.0.0.1",port,password,"设备 "+target,fp); });
            connect(tunnel,&RelayTunnel::error,owner,[this,owner](const QString &s){ if(channel==owner) failure(s); });
            connect(tunnel,&RelayTunnel::closed,owner,[this,owner]{ if(wanted && channel==owner) failure("中转连接已断开"); });
            tunnel->startViewer();
        };
        if(QSettings().value("network/forceRelay",false).toBool()) { relay(); return; }
        connect(agent,&RelayAgent::peerResolved,owner,[this,owner,gen,relay](const QJsonObject &j){
            if(j.value("target").toString()!=target || generation!=gen || channel!=owner) return;
            const auto fp=j.value("fingerprint").toString(); const int port=j.value("port").toInt(); QString host;
            for(const auto &address:j.value("lan").toArray()) if(localSubnet(address.toString())) { host=address.toString(); break; }
            if(host.isEmpty() || port<1 || port>65535 || fp.size()!=64) { relay(); return; }
            auto probe=new QSslSocket(owner);
            connect(probe,&QSslSocket::sslErrors,owner,[probe](const QList<QSslError> &errors){
                for(const auto &e:errors) if(e.error()!=QSslError::SelfSignedCertificate && e.error()!=QSslError::CertificateUntrusted && e.error()!=QSslError::HostNameMismatch && e.error()!=QSslError::UnableToGetLocalIssuerCertificate && e.error()!=QSslError::UnableToVerifyFirstCertificate) return;
                probe->ignoreSslErrors(errors);
            });
            connect(probe,&QSslSocket::encrypted,owner,[this,probe,owner,gen,host,port,fp,relay]{
                if(generation!=gen || channel!=owner) return;
                if(QString::fromLatin1(probe->peerCertificate().digest(QCryptographicHash::Sha256).toHex())!=fp) { probe->abort(); relay(); return; }
                ++generation; probe->abort(); endpoint="局域网直连"; client.connectTo(host,quint16(port),password,"设备 "+target,fp);
            });
            connect(probe,&QSslSocket::errorOccurred,owner,[relay](QAbstractSocket::SocketError){relay();});
            QTimer::singleShot(2000,owner,[probe,relay]{probe->abort();relay();});
            probe->connectToHostEncrypted(host,quint16(port));
        });
        agent->resolveDevice(target); QTimer::singleShot(5000,owner,relay);
    }
protected:
    void dragEnterEvent(QDragEnterEvent *e) override {
        if(client.active() && e->mimeData()->hasUrls()) for(const auto &url:e->mimeData()->urls()) if(url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {e->acceptProposedAction();return;}
    }
    void dropEvent(QDropEvent *e) override {
        if(!client.active()) return;QStringList files;
        for(const auto &url:e->mimeData()->urls()) if(url.isLocalFile()) files.append(url.toLocalFile());
        client.sendFiles(files);e->acceptProposedAction();
    }
    void contextMenuEvent(QContextMenuEvent *e) override {showMenu(e->globalPos());}
    void showMenu(const QPoint &position) {
        QMenu menu;auto send=menu.addAction("传送文件…");send->setEnabled(client.active());auto cancel=menu.addAction("取消文件发送");menu.addSeparator();
        auto restart=menu.addAction("重启远程电脑…"),shutdown=menu.addAction("关闭远程电脑…");
        restart->setEnabled(client.active() && control);shutdown->setEnabled(client.active() && control);
        auto selected=menu.exec(position);
        if(selected==send) client.sendFiles(QFileDialog::getOpenFileNames(this,"选择要传送的文件"));
        else if(selected==cancel) client.cancelFiles();
        else if(selected==restart || selected==shutdown) {
            const auto text=selected==restart ? "重启" : "关机";
            if(QMessageBox::question(this,"确认远程电源操作",QString("确定对 %1 执行%2？请先保存对方未保存的工作。").arg(name,text))==QMessageBox::Yes)
                client.requestPower(selected==restart ? "restart" : "shutdown");
        }
    }
signals:
    void editRequested();
    void removeRequested();
    void foregroundRequested();
    void stateChanged(const QString &text,int state,bool wanted);
};

ConnectionList::ConnectionList(RelayAgent *agent,QWidget *parent):QWidget(parent),m_agent(agent) {
    auto layout=new QVBoxLayout(this); layout->setContentsMargins(18,16,18,16);
    auto title=new QLabel("连接列表"); title->setObjectName("title"); layout->addWidget(title);
    auto hint=new QLabel("双击绿色缩略图操作远程电脑；关闭或最小化远程窗口后返回低带宽缩略图。<br/>"
        "<span style=\"color:#8b95a5\">●</span> 灰色：未连接 / 不在线　"
        "<span style=\"color:#e65252\">●</span> 红色：异常 / 无画面　"
        "<span style=\"color:#22b77a\">●</span> 绿色：正在接收画面"); hint->setWordWrap(true); layout->addWidget(hint);
    auto row=new QHBoxLayout; m_groups=new QComboBox; m_groups->setMinimumWidth(160); row->addWidget(m_groups,1);
    for(const auto &text:{"添加电脑","添加分组","重命名分组","删除分组"}) {
        auto button=new QPushButton(text); row->addWidget(button);
        if(QString(text)=="添加电脑") connect(button,&QPushButton::clicked,this,[this]{edit();});
        else if(QString(text)=="添加分组") connect(button,&QPushButton::clicked,this,[this]{
            bool ok=false; auto name=QInputDialog::getText(this,"添加分组","分组名称：",QLineEdit::Normal,{},&ok).trimmed();
            if(ok && !name.isEmpty() && name!="全部分组" && !m_groupNames.contains(name)) { m_groupNames.append(name.left(64)); refreshGroups(); save(); }
        });
        else if(QString(text)=="重命名分组") connect(button,&QPushButton::clicked,this,[this]{manageGroup(false);});
        else connect(button,&QPushButton::clicked,this,[this]{manageGroup(true);});
    }
    layout->addLayout(row);
    auto scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    m_gridWidget=new QWidget; m_grid=new QGridLayout(m_gridWidget); m_grid->setAlignment(Qt::AlignTop); m_grid->setSpacing(16); scroll->setWidget(m_gridWidget); layout->addWidget(scroll,1);
    m_groupNames=QSettings().value("connectionList/groups",QStringList{"默认分组"}).toStringList();
    if(!m_groupNames.contains("默认分组")) m_groupNames.prepend("默认分组");
    refreshGroups(); connect(m_groups,&QComboBox::currentTextChanged,this,[this]{reflow();});
    const auto records=QSettings().value("connectionList/devices").toJsonArray();
    for(const auto &value:records) {
        const auto j=value.toObject(); auto device=new DeviceCard(agent,m_gridWidget);
        device->uid=j.value("uid").toString(); device->target=j.value("target").toString(); device->name=j.value("name").toString(); device->group=j.value("group").toString("默认分组");
        device->remember=j.value("remember").toBool(); device->automatic=j.value("automatic").toBool(true); device->wanted=device->automatic && device->remember;
        if(device->uid.isEmpty()) device->uid=QUuid::createUuid().toString(QUuid::Id128);
        if(!m_groupNames.contains(device->group)) m_groupNames.append(device->group);
        m_devices.append(device);
    }
    for(auto d:m_devices) bind(d);
    refreshGroups(); reflow();
    connect(agent,&RelayAgent::onlineChanged,this,[this](bool online,const QString &){
        if(online) reconnectAll(); else for(auto d:m_devices) d->suspended();
    });
}
void ConnectionList::stopAll() { for(auto d:m_devices) d->stop(); }
void ConnectionList::reconnectAll() {
    int delay=0; for(auto d:m_devices) if(d->wanted) { QTimer::singleShot(delay,d,[d]{d->start();}); delay+=350; }
}
void ConnectionList::save() {
    QJsonArray records; for(auto d:m_devices) records.append(QJsonObject{{"uid",d->uid},{"target",d->target},{"name",d->name},{"group",d->group},{"remember",d->remember},{"automatic",d->automatic}});
    QSettings settings; settings.setValue("connectionList/devices",records); settings.setValue("connectionList/groups",m_groupNames);
}
void ConnectionList::refreshGroups() {
    const auto current=m_groups->currentText(); QSignalBlocker block(m_groups); m_groups->clear(); m_groups->addItem("全部分组"); m_groups->addItems(m_groupNames); m_groups->setCurrentText(current.isEmpty() ? "全部分组" : current);
}
void ConnectionList::reflow() {
    while(auto item=m_grid->takeAt(0)) { if(auto w=item->widget()) { if(w->property("groupHeading").toBool()) delete w; else w->hide(); } delete item; }
    const int columns=qMax(1,m_gridWidget->width()/290); int row=0;
    for(const auto &group:m_groupNames) {
        if(m_groups->currentIndex()>0 && m_groups->currentText()!=group) continue;
        if(std::none_of(m_devices.begin(),m_devices.end(),[&](DeviceCard *d){return d->group==group;})) continue;
        auto heading=new QLabel(group); heading->setProperty("groupHeading",true); heading->setStyleSheet("font-size:18px;font-weight:600;padding-top:10px;"); m_grid->addWidget(heading,row++,0,1,columns);
        int col=0;
        for(auto d:m_devices) if(d->group==group) { m_grid->addWidget(d,row,col); d->show(); if(++col==columns) {col=0;++row;} }
        if(col) ++row;
    }
}
void ConnectionList::resizeEvent(QResizeEvent *e) { QWidget::resizeEvent(e); QTimer::singleShot(0,this,[this]{reflow();}); }
void ConnectionList::manageGroup(bool remove) {
    auto old=m_groups->currentText(); if(m_groups->currentIndex()==0 || old=="默认分组") { QMessageBox::information(this,"分组","请先选择一个自定义分组。"); return; }
    if(remove) {
        if(QMessageBox::question(this,"删除分组","组内电脑将移入默认分组，是否继续？")!=QMessageBox::Yes) return;
        for(auto d:m_devices) if(d->group==old) d->group="默认分组"; m_groupNames.removeAll(old);
    } else {
        bool ok=false; auto name=QInputDialog::getText(this,"重命名分组","新的分组名称：",QLineEdit::Normal,old,&ok).trimmed().left(64);
        if(!ok || name.isEmpty() || name=="全部分组" || m_groupNames.contains(name)) return;
        for(auto d:m_devices) if(d->group==old) d->group=name; m_groupNames[m_groupNames.indexOf(old)]=name;
    }
    refreshGroups(); save(); reflow();
}
void ConnectionList::edit(DeviceCard *device) {
    QDialog dialog(this); dialog.setWindowTitle(device ? "修改电脑" : "添加电脑"); dialog.setMinimumWidth(440);
    auto layout=new QVBoxLayout(&dialog); auto form=new QFormLayout; layout->addLayout(form);
    QLineEdit name(device ? device->name : QString()),target(device ? device->target : QString()),password;
    name.setMaxLength(64); target.setMaxLength(11); password.setMaxLength(64); password.setEchoMode(QLineEdit::Password);
    password.setPlaceholderText(device ? "留空保留原密码" : "8–64 字符");
    QComboBox groups; groups.addItems(m_groupNames); groups.setCurrentText(device ? device->group : (m_groups->currentIndex()>0 ? m_groups->currentText() : "默认分组"));
    QCheckBox remember("使用系统加密保存密码"),automatic("下次启动自动连接（需保存密码）"); remember.setChecked(device ? device->remember : true); automatic.setChecked(device ? device->automatic : true);
    form->addRow("名称",&name); form->addRow("设备 ID",&target); form->addRow("连接密码",&password); form->addRow("分组",&groups); layout->addWidget(&remember); layout->addWidget(&automatic);
    QDialogButtonBox buttons(QDialogButtonBox::Save|QDialogButtonBox::Cancel); layout->addWidget(&buttons); connect(&buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(&buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        auto id=target.text().remove(' '); QString code=password.text();
        if(device && code.isEmpty()) code=device->password.isEmpty() && device->remember ? Platform::loadPassword(device->account()) : device->password;
        const bool keepingEmpty=device && id==device->target && code.isEmpty() && !remember.isChecked();
        if(!QRegularExpression("^[0-9]{9}$").match(id).hasMatch() || (!keepingEmpty && (code.size()<8 || code.size()>64)) || name.text().trimmed().isEmpty()) { QMessageBox::warning(&dialog,"输入无效","请输入名称、九位设备 ID 和 8–64 字符的密码。"); return; }
        for(auto d:m_devices) if(d!=device && d->target==id) { QMessageBox::warning(&dialog,"设备重复","该设备已在连接列表中。"); return; }
        bool created=!device;
        if(created) { device=new DeviceCard(m_agent,m_gridWidget); device->uid=QUuid::createUuid().toString(QUuid::Id128); }
        if(remember.isChecked() && !Platform::savePassword(device->account(),code)) { QMessageBox::warning(&dialog,"保存失败","系统无法保存密码。请取消保存密码后重试。"); if(created){delete device;device=nullptr;} return; }
        if(!remember.isChecked()) Platform::removePassword(device->account());
        device->stop(); device->name=name.text().trimmed(); device->target=id; device->group=groups.currentText(); device->password=code; device->remember=remember.isChecked(); device->automatic=automatic.isChecked();
        if(created) {
            m_devices.append(device); bind(device);
        }
        device->updateCaption(); device->wanted=!code.isEmpty(); device->start(); save(); reflow(); dialog.accept();
    });
    dialog.exec();
}
#include "connectionlist.moc"

void ConnectionList::bind(DeviceCard *d) {
        d->updateCaption();
        connect(d,&DeviceCard::editRequested,this,[this,d]{edit(d);});
        connect(d,&DeviceCard::removeRequested,this,[this,d]{
            if(QMessageBox::question(this,"删除电脑","删除「"+d->name+"」及本列表保存的密码？")==QMessageBox::Yes) { d->stop(); Platform::removePassword(d->account()); m_devices.removeAll(d); delete d; save(); reflow(); }
        });
        connect(d,&DeviceCard::foregroundRequested,this,[this,d]{for(auto other:m_devices) if(other!=d && (other->expanded || other->viewer->isVisible())) other->viewer->preview();});

    connect(d,&DeviceCard::stateChanged,this,[this,d](const QString &text,int state,bool wanted){emit connectionState(d->target,text,state,wanted);});
}

bool ConnectionList::wantsConnection(const QString &target) const {
    for(auto d:m_devices) if(d->target==target) return d->wanted;
    return false;
}
void ConnectionList::disconnectDesktop(const QString &target) { for(auto d:m_devices) if(d->target==target) d->stop(); }
bool ConnectionList::connectDesktop(const QString &target,const QString &password,bool remember) {
    DeviceCard *device=nullptr;
    for(auto d:m_devices) if(d->target==target) { device=d; break; }
    const bool created=!device;
    if(created) { device=new DeviceCard(m_agent,m_gridWidget); device->uid=QUuid::createUuid().toString(QUuid::Id128); device->target=target; device->name="设备 "+target; device->group="默认分组"; }
    if(remember && !Platform::savePassword(device->account(),password)) { if(created) delete device; return false; }
    if(!remember) Platform::removePassword(device->account());
    if(created) { m_devices.append(device); bind(device); }
    device->password=password; device->remember=remember; device->wanted=true; device->openWhenReady=true;
    device->updateCaption(); save(); reflow();
    if(device->client.active() && device->preview->state==2) { device->openWhenReady=false; device->viewer->begin(device->name,device->control); }
    else device->start();
    return true;
}
