#include "mainwindow.h"
#include "adminpassword.h"
#include "systemsettings.h"
#include "exitdeadline.h"
#include "trayicon.h"
#include <QSystemTrayIcon>
#include <QMenu>
#include <QSettings>
#include "viewer.h"
#include "connectionlist.h"
#include "platform.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonDocument>
#include <QInputDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDir>
#include <QTimer>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QStyle>

static QLabel *label(const QString &text,const char *style=nullptr) {
    auto l=new QLabel(text); l->setWordWrap(true); if (style) l->setObjectName(style); return l;
}
static QFrame *card(QVBoxLayout **layout) {
    auto frame=new QFrame; frame->setObjectName("card"); *layout=new QVBoxLayout(frame);
    (*layout)->setContentsMargins(16,14,16,14); (*layout)->setSpacing(9); return frame;
}
static QString configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)+"/relay-config.json";
}
static RelayConfig parseConfig(const QJsonObject &j) {
    RelayConfig c; c.host=j.value("server").toString().trimmed();
    int port=j.value("port").toInt(443); c.port=port>0 && port<=65535 ? quint16(port) : 0;
    c.fingerprint=RelayConfig::normalizePin(j.value("fingerprint").toString());
    c.enrollmentKey=j.value("enrollmentKey").toString(); return c;
}
static RelayConfig readConfig() {
    QFile file(configPath());
    if (!file.exists()) file.setFileName(Platform::bundledConfigPath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    return parseConfig(QJsonDocument::fromJson(file.readAll()).object());
}
MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent),m_host(this),m_relay(this) {
    QString startupStorageError;
    connect(&m_host,&RemoteHost::fileReceived,this,[this](const QString &path){
        auto prompt=new QMessageBox(QMessageBox::Information,"文件接收完成",QFileInfo(path).fileName()+" 已保存。是否打开所在文件夹？",QMessageBox::Yes|QMessageBox::No,this);
        prompt->setTextFormat(Qt::PlainText);prompt->setAttribute(Qt::WA_DeleteOnClose);prompt->setWindowModality(Qt::NonModal);
        connect(prompt,&QMessageBox::finished,this,[path](int answer){if(answer==QMessageBox::Yes)QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));});prompt->show();
    });
    qApp->installEventFilter(this);
    if(!qApp->property("linkdesk.previewMode").toBool()) {
        qApp->setQuitOnLastWindowClosed(false);
        auto icon=Branding::trayIcon();
        m_tray=new QSystemTrayIcon(icon,this);m_tray->setObjectName("linkdeskTray");m_tray->setToolTip("LinkDesk · 远程桌面");
        auto menu=new QMenu(this);auto restore=menu->addAction("显示界面");restore->setObjectName("trayShowAction");
        menu->addSeparator();auto exit=menu->addAction("退出");exit->setObjectName("trayExitAction");
        connect(restore,&QAction::triggered,this,&MainWindow::restoreFromTray);connect(exit,&QAction::triggered,this,&MainWindow::requestExit);
        connect(m_tray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason reason){if(reason==QSystemTrayIcon::Trigger || reason==QSystemTrayIcon::DoubleClick)restoreFromTray();});
        m_tray->setContextMenu(menu);m_tray->show();
    }
    connect(qApp,&QCoreApplication::aboutToQuit,this,[this]{beginExit();});
    QSettings adminSettings; AdminPassword::initialize(adminSettings);
    setWindowTitle("LinkDesk · 远程桌面"); resize(1080,740); setMinimumSize(860,560);
    setStyleSheet(R"(
      QMainWindow, QWidget#content { background:#f3f6fb; }
      QWidget { font-family:'PingFang SC','Microsoft YaHei',sans-serif; font-size:13px; color:#24324b; }
      QFrame#sidebar { background:#101b32; border:0; }
      #sidebar QLabel { color:#9caecb; } #sidebar QLabel#brand {color:white;font-size:25px;font-weight:700;}
      #sidebar QPushButton {text-align:left; background:transparent;color:#a9b9d2;border:0;padding:9px 18px;border-radius:10px;}
      #sidebar QPushButton#navActive {background:#263e67;color:#fff;font-weight:600;}
      QFrame#card {background:white;border:1px solid #e5ebf4;border-radius:16px;}
      QLabel#title {font-size:24px;font-weight:700;color:#15243e;}
      QLabel#section {font-size:20px;font-weight:650;color:#15243e;}
      QLabel#muted {color:#8290a6;font-size:13px;}
      QLabel#bigCode {font-size:34px;font-weight:700;color:#2464e9;letter-spacing:5px;}
      QLabel#address {font-size:18px;font-weight:600;color:#243b5f;}
      QLabel#pill {color:#317d5b;background:#e9f7ef;border-radius:12px;padding:7px 12px;font-size:12px;}
      QLineEdit, QSpinBox {background:#f8faff;border:1px solid #dce4f0;border-radius:9px;padding:8px;min-height:22px;selection-background-color:#2865e8;}
      QLineEdit:focus, QSpinBox:focus {border:1px solid #2865e8;background:white;}
      QPushButton {background:#edf2fa;border:0;border-radius:9px;padding:8px 16px;min-height:20px;color:#385274;font-weight:600;}
      QPushButton:hover {background:#e1eafa;} QPushButton:disabled {background:#edf0f5;color:#a3adbd;}
      QPushButton#primary {background:#2865e8;color:white;padding:9px;font-size:15px;}
      QPushButton#primary:hover {background:#1b53d0;} QPushButton#primary:disabled {background:#adc3ef;}
      QPushButton#danger {background:#fff0ef;color:#be4b48;}
      QCheckBox {spacing:9px;} QCheckBox::indicator {width:18px;height:18px;}
      QLabel#banner {background:#eaf1ff;border:1px solid #d9e5ff;border-radius:10px;color:#426392;padding:13px;}
    )");
    auto root=new QWidget; auto row=new QHBoxLayout(root); row->setContentsMargins(0,0,0,0); row->setSpacing(0); setCentralWidget(root);
    auto sidebar=new QFrame; sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(180);
    auto nav=new QVBoxLayout(sidebar); nav->setContentsMargins(16,18,16,12); nav->setSpacing(8);
    nav->addWidget(label("▣  LinkDesk","brand")); nav->addWidget(label("连接，让距离更近","muted")); nav->addSpacing(18);
    auto remote=new QPushButton("◈   远程连接"); remote->setObjectName("navActive"); nav->addWidget(remote);
    auto listButton=new QPushButton("▦   连接列表"); nav->addWidget(listButton);
    auto help=new QPushButton("◎   使用指南"); nav->addWidget(help);
    auto serverSettings=new QPushButton("◇   软件设置"); serverSettings->setObjectName("softwareSettingsButton"); nav->addWidget(serverSettings);
    connect(serverSettings,&QPushButton::clicked,this,&MainWindow::configureServer);
    auto exit=new QPushButton("⏻   完全退出");exit->setObjectName("completeExitButton");nav->addWidget(exit);
    connect(exit,&QPushButton::clicked,this,&MainWindow::requestExit);
    nav->addStretch(); nav->addWidget(label("C++ / Qt 6\n互联网中继 · v"+QCoreApplication::applicationVersion(),"muted")); row->addWidget(sidebar);
    connect(help,&QPushButton::clicked,this,[this] {
        QMessageBox::information(this,"使用指南",
          "1. 两台电脑打开软件；预配置版本会自动连接服务器。\n"
          "2. 等待上线，软件会取得稳定的九位设备 ID。\n"
          "3. 被控端开启共享，提供设备 ID 和临时密码。\n"
          "4. 主控端输入 ID 和密码，即可跨网络连接。\n"
          "5. 被控端开启鼠标键盘控制后才接受操作。\n\n"
          + Platform::permissionHelp()+"\n" +
          "共享开关会记住，下次启动可自动共享。鼠标控制会记住你的选择；Mac 仍需系统授权。\n"
          "支持主屏幕与常用英文按键，支持拖放文件到连接列表缩略图；暂不支持中文输入法或音频。\n"
          "Ctrl+Shift+Esc 退出全屏，窗口模式下断开连接。");
    });
    auto content=new QWidget; content->setObjectName("content"); content->setMinimumHeight(620);
    auto scroll=new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto pages=new QStackedWidget; scroll->setWidget(content); pages->addWidget(scroll); row->addWidget(pages,1);
    m_list=new ConnectionList(&m_relay); pages->addWidget(m_list);
    auto navigate=[remote,listButton,pages](int index) {
        pages->setCurrentIndex(index); remote->setObjectName(index==0 ? "navActive" : ""); listButton->setObjectName(index==1 ? "navActive" : "");
        for(auto button:{remote,listButton}) { button->style()->unpolish(button); button->style()->polish(button); }
    };
    connect(remote,&QPushButton::clicked,this,[navigate]{navigate(0);});
    connect(listButton,&QPushButton::clicked,this,[navigate]{navigate(1);});
    auto body=new QVBoxLayout(content); body->setContentsMargins(18,16,18,16); body->setSpacing(12);
    auto header=new QHBoxLayout; auto titles=new QVBoxLayout;
    titles->addWidget(label("远程连接","title")); titles->addWidget(label("安全连接另一台电脑，或将此设备共享给对方。","muted"));
    header->addLayout(titles,1);
    m_networkState=label("●  未连接服务器","pill"); m_networkState->setFixedHeight(32); header->addWidget(m_networkState);
    body->addLayout(header);
    auto cards=new QHBoxLayout; cards->setSpacing(20);
    QVBoxLayout *local; auto localCard=card(&local); local->setSpacing(9); cards->addWidget(localCard,1);
    local->addWidget(label("此设备","section")); local->addWidget(label(QHostInfo::localHostName(),"muted"));
    local->addSpacing(8); local->addWidget(label("本机设备 ID","muted"));
    m_address=label("尚未注册","address"); m_address->setMinimumHeight(40);
    m_address->setStyleSheet("font-size:24px;font-weight:700;");
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse); local->addWidget(m_address);
    auto copy=new QPushButton("复制设备 ID"); copy->setMaximumWidth(140); local->addWidget(copy);
    connect(copy,&QPushButton::clicked,this,[this] {
        if (m_relay.deviceId().isEmpty()) return;
        QApplication::clipboard()->setText(m_relay.deviceId()); message("本机设备 ID 已复制");
    });
    local->addWidget(label("连接密码","muted")); auto pinrow=new QHBoxLayout;
    m_code=label(m_host.code(),"bigCode"); m_code->setMinimumHeight(48); m_code->setWordWrap(true);
    m_code->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    m_code->setStyleSheet("font-size:24px;letter-spacing:2px;");
    m_code->setTextInteractionFlags(Qt::TextSelectableByMouse); pinrow->addWidget(m_code,1);
    auto customize=new QPushButton("自定义"); pinrow->addWidget(customize);
    auto rotate=new QPushButton("刷新"); pinrow->addWidget(rotate); local->addLayout(pinrow);
    connect(rotate,&QPushButton::clicked,this,[this] {
        m_relay.endHosting(); m_host.rotateCode(); m_code->setText(m_host.code());
        if (m_saveHostPassword->isChecked() && !Platform::savePassword("host-password",m_host.code())) { message("密码已刷新，但保存到系统失败。",true); return; }
        message("连接密码已更新，现有会话已断开。");
    });
    m_saveHostPassword=new QCheckBox("保存本机连接密码（下次启动继续使用）"); local->addWidget(m_saveHostPassword);
    m_saveHostPassword->setObjectName("saveHostPassword");
    m_saveHostPassword->setChecked(QSettings().value("saveHostPassword",true).toBool());
    if(m_saveHostPassword->isChecked() && !qApp->property("linkdesk.previewMode").toBool()) {
        const auto saved=Platform::loadPassword("host-password");QString error;
        if(!saved.isEmpty() && m_host.setCode(saved,&error)) m_code->setText(m_host.code());
        else if(!Platform::savePassword("host-password",m_host.code())) {
            m_saveHostPassword->setChecked(false);startupStorageError="本机密码加密保存失败，请检查本机数据目录权限。";
        }
    }
    connect(m_saveHostPassword,&QCheckBox::toggled,this,[this](bool enabled) {
        bool ok=enabled ? Platform::savePassword("host-password",m_host.code()) : Platform::removePassword("host-password");
        if (!ok) { QSignalBlocker blocker(m_saveHostPassword); m_saveHostPassword->setChecked(!enabled); message("无法更新系统保存的本机密码。",true); return; }
        QSettings().setValue("saveHostPassword",enabled);
    });
    connect(customize,&QPushButton::clicked,this,[this] {
        bool accepted=false; const auto code=QInputDialog::getText(this,"自定义连接密码","请输入 8–64 个字符的密码（区分大小写）：",QLineEdit::Password,{},&accepted);
        if (!accepted) return;
        QString error; if (!m_host.setCode(code,&error)) { message(error,true); return; }
        m_relay.endHosting(); m_code->setText(code);
        if (m_saveHostPassword->isChecked() && !Platform::savePassword("host-password",code)) { message("密码已修改，但保存到系统失败。",true); return; }
        message("连接密码已自定义，现有会话已断开。");
    });
    local->addWidget(label("连接服务器","muted"));
    m_server=label("请先打开服务器设置","muted"); local->addWidget(m_server);
    m_control=new QCheckBox("允许鼠标键盘控制"); local->addWidget(m_control);
    m_control->setObjectName("allowRemoteControl");
    m_control->setToolTip("未勾选时，对方只能观看。记住你的选择；Mac 仍需系统辅助功能授权。");
    m_control->setChecked(QSettings().value("controlEnabled",true).toBool());
    connect(m_control,&QCheckBox::toggled,this,[this](bool enabled) {
        QSettings().setValue("controlEnabled",enabled);
        QString error;
        if (!m_host.setControl(enabled,&error)) {
            QSignalBlocker blocker(m_control); m_control->setChecked(false); message(error,true);
        }
        if (m_disconnectHost->isEnabled()) {
            auto name=m_peer->text().section(" · ",0,0);
            m_peer->setText(name+" · "+(m_control->isChecked() ? "可控制" : "仅观看"));
        }
    });
    local->addWidget(label("开启后，持有 ID 和密码的设备可申请连接。","muted"));
    local->addStretch(); m_share=new QPushButton("开启共享"); m_share->setObjectName("primary"); local->addWidget(m_share);
    m_hostStatus=label("●  共享未开启","muted"); local->addWidget(m_hostStatus);
    connect(m_share,&QPushButton::clicked,this,&MainWindow::toggleShare);
    QVBoxLayout *other; auto otherCard=card(&other); other->setSpacing(12); cards->addWidget(otherCard,1);
    other->addWidget(label("连接远程设备","section"));
    other->addWidget(label("局域网或互联网，使用同一个设备 ID","muted")); other->addSpacing(8);
    other->addWidget(label("远程设备 ID","muted"));
    m_target=new QLineEdit; m_target->setObjectName("remoteDeviceId"); m_target->setPlaceholderText("例如 123 456 789");
    m_target->setClearButtonEnabled(true); m_target->setMaxLength(11);
    m_target->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9 ]{0,11}"),m_target)); other->addWidget(m_target);
    other->addWidget(label("连接密码","muted"));
    m_password=new QLineEdit; m_password->setObjectName("remotePassword"); m_password->setPlaceholderText("输入对方的连接密码（8–64 字符）");
    m_password->setEchoMode(QLineEdit::Password); m_password->setMaxLength(64); other->addWidget(m_password);
    auto showCode=new QCheckBox("显示密码"); other->addWidget(showCode);
    connect(showCode,&QCheckBox::toggled,this,[this](bool show) { m_password->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password); });
    m_rememberPassword=new QCheckBox("保存此设备的连接密码"); m_rememberPassword->setChecked(true);other->addWidget(m_rememberPassword);
    m_rememberPassword->setToolTip("Mac 使用本机加密保存，Windows 使用当前用户的 DPAPI 加密。取消勾选会删除已保存的密码。");
    connect(m_target,&QLineEdit::textChanged,this,[this] {
        const auto target=m_target->text().remove(' '); m_password->clear();
        QSignalBlocker blocker(m_rememberPassword);
        m_rememberPassword->setChecked(QSettings().value("rememberRemotePassword/"+target,true).toBool());
        if(!m_rememberPassword->isChecked() || !QRegularExpression("^[0-9]{9}$").match(target).hasMatch()) return;
        const auto saved=Platform::loadPassword(passwordAccount(target));
        if(!saved.isEmpty()) m_password->setText(saved);
    });
    connect(m_rememberPassword,&QCheckBox::toggled,this,[this](bool enabled) {
        const auto target=m_target->text().remove(' ');
        if(QRegularExpression("^[0-9]{9}$").match(target).hasMatch()) QSettings().setValue("rememberRemotePassword/"+target,enabled);
        if (!enabled && QRegularExpression("^[0-9]{9}$").match(target).hasMatch() && !Platform::removePassword(passwordAccount(target))) message("无法删除系统保存的密码。",true);
    });
    other->addStretch(); other->addWidget(label("服务器自动定位设备，密码在两端之间验证。","muted"));
    m_connect=new QPushButton("连接远程设备  →"); m_connect->setObjectName("primary"); other->addWidget(m_connect);
    other->addWidget(label("端到端 TLS · 控制权限由受控端设置 · 画质与帧率可调","muted"));
    connect(m_connect,&QPushButton::clicked,this,&MainWindow::connectRemote);
    connect(m_password,&QLineEdit::returnPressed,this,&MainWindow::connectRemote);
    body->addLayout(cards,1);
    QVBoxLayout *session; auto sessionCard=card(&session); session->setContentsMargins(22,18,22,18);
    auto sessionRow=new QHBoxLayout; auto sessionText=new QVBoxLayout;
    sessionText->addWidget(label("共享会话","section")); m_peer=label("暂无设备连接","muted"); sessionText->addWidget(m_peer); sessionRow->addLayout(sessionText,1);
    m_disconnectHost=new QPushButton("结束共享会话"); m_disconnectHost->setObjectName("danger"); m_disconnectHost->setEnabled(false); sessionRow->addWidget(m_disconnectHost); session->addLayout(sessionRow);
    connect(m_disconnectHost,&QPushButton::clicked,this,[this] { m_host.disconnectPeer(); m_relay.endHosting(); });
    m_fingerprint=label("开启共享后，将设备 ID 和临时密码提供给对方。","muted"); m_fingerprint->setTextInteractionFlags(Qt::TextSelectableByMouse); session->addWidget(m_fingerprint); body->addWidget(sessionCard);
    m_banner=label("准备就绪。配置服务器后，即可跨网络连接。"); m_banner->setObjectName("banner"); body->addWidget(m_banner);
    connect(&m_host,&RemoteHost::status,this,[this](const QString &s) { m_hostStatus->setText("●  "+s); });
    connect(&m_host,&RemoteHost::peerChanged,this,[this](bool connected,const QString &name) {
        m_disconnectHost->setEnabled(connected); m_peer->setText(connected ? (m_hostViewer.isEmpty() ? name : m_hostViewer)+" · "+(m_control->isChecked() ? "可控制" : "仅观看") : "暂无设备连接");
    });
    connect(m_list,&ConnectionList::connectionState,this,[this](const QString &target,const QString &text,int state,bool wanted) {
        if(target!=m_connectingTarget) return;
        m_connect->setEnabled(true); m_connect->setText(wanted ? "断开远程连接" : "连接远程设备  →");
        message(text,state==1);
    });
    connect(&m_relay,&RelayAgent::incomingViewer,this,[this](const QString &id) { m_hostViewer=id; });
    connect(&m_relay,&RelayAgent::onlineChanged,this,[this](bool online,const QString &id) {
        m_address->setText(id.isEmpty() ? "尚未注册" : id.left(3)+" "+id.mid(3,3)+" "+id.right(3));
        m_networkState->setText(online ? "●  服务器在线" : "●  服务器未连接");
        m_networkState->setStyleSheet(online ? "color:#31856b;background:#ebf7ef;border-radius:16px;padding:8px 16px;" : "color:#a46519;background:#fff4dd;border-radius:16px;padding:8px 16px;");
        m_server->setText(m_relay.config().host.isEmpty() ? "请先打开服务器设置" : m_relay.config().host+":"+QString::number(m_relay.config().port));
        if (m_host.isListening()) m_hostStatus->setText(online ? "●  共享已开启，等待连接" : "●  共享已准备，等待服务器重连");
    });
    connect(&m_relay,&RelayAgent::status,this,[this](const QString &s) { message(s); });
    connect(&m_relay,&RelayAgent::error,this,[this](const QString &s) { message(s,true); });
    m_target->setText(QSettings().value("lastDeviceId").toString());
    if(!startupStorageError.isEmpty()) message(startupStorageError,true);
    if(!qApp->property("linkdesk.previewMode").toBool())
        QTimer::singleShot(0,this,&MainWindow::startConfiguredServer);
}
void MainWindow::message(const QString &text,bool error) {
    m_banner->setText(text); m_banner->setStyleSheet(error ? "background:#fff1ee;color:#ad4944;border-color:#f5d5d0;" : "");
}
void MainWindow::toggleShare() {
    if (m_host.isListening()) {
        m_shareRetry.stop(); m_relay.setAvailable(0); m_host.stop(); QSettings().setValue("shareEnabled",false);
        m_share->setText("开启共享"); m_hostStatus->setText("●  共享未开启"); message("屏幕共享已关闭。"); return;
    }
    if (!m_relay.config().valid()) { configureServer(); if (!m_relay.config().valid()) return; }
    QString error;if(!startSharing(&error)) {message(error,true);return;}
    QSettings().setValue("shareEnabled",true);
}
bool MainWindow::startSharing(QString *error) {
    if(m_host.isListening())return true;
    QString controlError;
    const bool wanted=QSettings().value("controlEnabled",true).toBool();
    const bool allowed=m_host.setControl(wanted,&controlError);
    {QSignalBlocker blocker(m_control);m_control->setChecked(wanted && allowed);}
    if(!m_host.start(45432,error,false,QHostAddress::Any) && !m_host.start(0,error,false,QHostAddress::Any))return false;
    m_relay.setAvailable(m_host.port(),m_host.fingerprint());
    m_share->setText("关闭共享");m_fingerprint->setText("本机证书 SHA-256\n"+m_host.fingerprint());
    message(controlError.isEmpty() ? "共享已开启。请将设备 ID 和连接密码提供给对方。" : "共享已开启，仅观看："+controlError,!controlError.isEmpty());
    return true;
}

void MainWindow::connectRemote() {
    if (m_list->wantsConnection(m_connectingTarget)) { m_list->disconnectDesktop(m_connectingTarget); message("远程连接已断开。"); return; }
    const auto target=m_target->text().remove(' ');
    if (!QRegularExpression("^[0-9]{9}$").match(target).hasMatch()) { message("请输入对方的九位设备 ID。",true); return; }
    if (m_password->text().size()<8 || m_password->text().size()>64) { message("请输入对方的连接密码（8–64 个字符）。",true); return; }
    if (target==m_relay.deviceId()) { message("请使用另一台电脑 的设备 ID。",true); return; }
    QSettings().setValue("lastDeviceId",target); m_connect->setEnabled(false);
    m_connect->setText("正在连接…"); message("正在建立端到端加密连接…");
    m_connectingTarget=target;
    if(!m_list->connectDesktop(target,m_password->text(),m_rememberPassword->isChecked())) {
        message("系统无法保存密码，请取消保存密码后重试。",true); m_connect->setEnabled(true); m_connect->setText("连接远程设备  →"); return;
    }
    if(m_rememberPassword->isChecked()) Platform::savePassword(passwordAccount(target),m_password->text());
    else m_password->clear();
    m_connect->setEnabled(true); m_connect->setText("断开远程连接");
}
MainWindow::~MainWindow() {
    qApp->removeEventFilter(this);
    for(auto transfer:findChildren<FileTransfer *>()) transfer->shutdown();
    m_shareRetry.stop();m_list->stopAll();m_relay.stop();m_host.stop();
}
bool MainWindow::eventFilter(QObject *object,QEvent *event) {
    if(object==qApp && event->type()==QEvent::Quit && !m_exiting && !qApp->property("linkdesk.previewMode").toBool()) {
        requestExit();return true;
    }
    return QMainWindow::eventFilter(object,event);
}
void MainWindow::hideToTray() {
    if(m_inTray) return;
    m_inTray=true;m_savedState=windowState() & ~Qt::WindowMinimized;m_hiddenWindows.clear();
    for(auto widget:QApplication::topLevelWidgets()) if(widget!=this && isAncestorOf(widget) && widget->isVisible() && !widget->isMinimized() && !qobject_cast<QMenu *>(widget)) {
        for(auto canvas:widget->findChildren<RemoteCanvas *>()) canvas->releaseInput();
        m_hiddenWindows.append(widget);widget->hide();
    }
    hide();
}
void MainWindow::restoreFromTray() {
    if(m_exiting) return;
    if(m_inTray) {setWindowState(m_savedState);m_inTray=false;}
    else if(isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();raise();activateWindow();
    for(const auto &window:m_hiddenWindows) if(window) {
        auto canvas=window->findChild<RemoteCanvas *>();
        if(!canvas || canvas->property("encodedFrameSize").toSize().isValid()) window->show();
    }
    m_hiddenWindows.clear();
}
void MainWindow::requestExit() {
    if(m_exiting || m_confirmingExit) return;
    m_confirmingExit=true;
    QMessageBox confirm(QMessageBox::Question,"完全退出 LinkDesk","退出将结束所有远程连接、共享和文件传输。确定完全退出？",QMessageBox::Yes|QMessageBox::No,this);
    confirm.setObjectName("exitConfirmation");confirm.setDefaultButton(QMessageBox::No);
    confirm.button(QMessageBox::Yes)->setText("完全退出");confirm.button(QMessageBox::No)->setText("取消");
    const bool accepted=confirm.exec()==QMessageBox::Yes;m_confirmingExit=false;
    if(accepted) beginExit();
}
void MainWindow::beginExit() {
    if(m_exiting) return;
    m_exiting=true;
    if(!qApp->property("linkdesk.previewMode").toBool()) startExitDeadline();
    // Stop producers before disconnecting transports. Hash workers cooperate
    // with cancellation and retain flushed receiver partials for resuming.
    for(auto transfer:findChildren<FileTransfer *>()) transfer->shutdown();
    QSettings().sync();
    m_list->stopAll();m_relay.stop();m_host.stop();
    if(m_tray) m_tray->hide();
    QCoreApplication::quit();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if(m_exiting || qApp->property("linkdesk.previewMode").toBool()) {event->accept();beginExit();return;}
    event->ignore();hideToTray();
}
void MainWindow::restoreSharing() {
    if(m_exiting || !QSettings().value("shareEnabled",true).toBool() || !m_relay.config().valid()) { m_shareRetry.stop(); return; }
    QString error;
    if(startSharing(&error)) { m_shareRetry.stop(); return; }
    message(error,true);
#ifdef Q_OS_WIN
    if(!m_shareRetry.isActive()) m_shareRetry.start(5000);
#endif
}
void MainWindow::startConfiguredServer() {
    auto config=readConfig(); if (!config.valid()) return;
    m_relay.start(config);
    connect(&m_shareRetry,&QTimer::timeout,this,&MainWindow::restoreSharing,Qt::UniqueConnection);
    restoreSharing();
}
void MainWindow::configureServer() {
    QDialog dialog(this); dialog.setWindowTitle("软件设置"); dialog.setMinimumWidth(620);
    auto outer=new QVBoxLayout(&dialog); auto tabs=new QTabWidget; outer->addWidget(tabs);
    auto network=new QWidget; auto networkLayout=new QVBoxLayout(network);
    auto forceRelay=new QCheckBox("强制走中转服务器（关闭局域网直连）"); forceRelay->setChecked(QSettings().value("network/forceRelay",false).toBool());
    networkLayout->addWidget(forceRelay); networkLayout->addWidget(label("应用于远程连接和连接列表中的全部电脑。保存后现有连接将重新建立。","muted")); networkLayout->addStretch(); tabs->addTab(network,"连接选项");
    auto general=new QWidget;auto generalLayout=new QFormLayout(general);tabs->insertTab(0,general,"常规");tabs->setCurrentWidget(general);
    auto autoStart=new QCheckBox("登录系统后自动启动 LinkDesk");autoStart->setChecked(SystemSettings::autoStartEnabled());generalLayout->addRow(autoStart);
    auto receiveFolder=new QLineEdit(FileTransfer::receiveDirectory());auto chooseFolder=new QPushButton("选择…");auto folderRow=new QHBoxLayout;folderRow->addWidget(receiveFolder);folderRow->addWidget(chooseFolder);generalLayout->addRow("接收文件保存到",folderRow);
    generalLayout->addRow(label("默认为系统下载目录。将文件拖到已连接的缩略图即可发送；中断后重新连接会自动续传。","muted"));
    connect(chooseFolder,&QPushButton::clicked,&dialog,[&]{auto path=QFileDialog::getExistingDirectory(&dialog,"接收目录",receiveFolder->text());if(!path.isEmpty()) receiveFolder->setText(path);});
    auto serverPage=new QWidget; tabs->addTab(serverPage,"服务器设置");
    auto layout=new QVBoxLayout(serverPage); layout->setContentsMargins(24,24,24,24); layout->setSpacing(16);
    auto lockState=label("服务器设置已锁定，输入管理员密码后才能修改或导入配置。","muted"); layout->addWidget(lockState);
    auto unlock=new QPushButton("输入管理员密码解锁"); unlock->setObjectName("serverUnlockButton"); layout->addWidget(unlock);
    auto editor=new QWidget; editor->setObjectName("serverEditor"); auto editorLayout=new QVBoxLayout(editor); editorLayout->setContentsMargins(0,0,0,0); layout->addWidget(editor);
    editorLayout->addWidget(label("两台电脑只需配置一次同一台服务器。部署后可导入 linkdesk-server.json。","muted"));
    auto form=new QFormLayout; const auto config=readConfig();
    auto host=new QLineEdit(config.host); host->setObjectName("serverHost"); host->setPlaceholderText("服务器公网 IP 或域名");
    auto port=new QSpinBox; port->setRange(1,65535); port->setValue(config.port);
    auto pin=new QLineEdit(config.fingerprint); pin->setPlaceholderText("SHA-256 指纹（自签名服务器必须填写）");
    auto key=new QLineEdit(config.enrollmentKey); key->setEchoMode(QLineEdit::Password); key->setPlaceholderText("部署时生成的服务器接入密钥");
    form->addRow("服务器",host); form->addRow("TLS 端口",port); form->addRow("证书指纹",pin); form->addRow("接入密钥",key); editorLayout->addLayout(form);
    auto import=new QPushButton("导入服务器配置文件"); import->setObjectName("serverImportButton"); editorLayout->addWidget(import);
    bool serverUnlocked=false; editor->setEnabled(false);
    QString unlockedCredential;
    auto unlocked=[&] { return serverUnlocked && unlockedCredential==QSettings().value("administrator/credential").toString(); };
    connect(unlock,&QPushButton::clicked,&dialog,[&] {
        bool ok=false; auto password=QInputDialog::getText(&dialog,"管理员验证","请输入管理员密码：",QLineEdit::Password,{},&ok); if(!ok) return;
        QSettings settings;
        if(!AdminPassword::verify(settings,password)) { QMessageBox::warning(&dialog,"验证失败","管理员密码不正确。"); return; }
        serverUnlocked=true; unlockedCredential=settings.value("administrator/credential").toString(); editor->setEnabled(true); unlock->setEnabled(false);
        lockState->setText("已解锁。本次设置窗口关闭后自动重新锁定。");
    });
    connect(import,&QPushButton::clicked,&dialog,[&] {
        if(!unlocked()) return;
        auto path=QFileDialog::getOpenFileName(&dialog,"导入配置",{},"JSON (*.json)"); if (path.isEmpty()) return;
        QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size()>16384) { QMessageBox::warning(&dialog,"导入失败","配置文件无法读取或过大。"); return; }
        auto imported=parseConfig(QJsonDocument::fromJson(file.readAll()).object()); QString error;
        if (!imported.valid(&error)) { QMessageBox::warning(&dialog,"导入失败",error); return; }
        host->setText(imported.host); port->setValue(imported.port); pin->setText(imported.fingerprint); key->setText(imported.enrollmentKey);
    });
    auto adminPage=new QWidget; tabs->addTab(adminPage,"管理员密码"); auto adminLayout=new QVBoxLayout(adminPage);
    adminLayout->addWidget(label("首次使用默认管理员密码为 888888。修改密码需要提供当前管理员密码。","muted"));
    auto adminForm=new QFormLayout; auto current=new QLineEdit,newPassword=new QLineEdit,confirm=new QLineEdit;
    current->setObjectName("adminCurrentPassword"); newPassword->setObjectName("adminNewPassword"); confirm->setObjectName("adminConfirmPassword");
    for(auto field:{current,newPassword,confirm}) { field->setEchoMode(QLineEdit::Password); field->setMaxLength(64); }
    adminForm->addRow("当前管理员密码",current); adminForm->addRow("新管理员密码（6–64 字符）",newPassword); adminForm->addRow("确认新密码",confirm); adminLayout->addLayout(adminForm);
    auto change=new QPushButton("修改管理员密码"); change->setObjectName("adminChangeButton"); adminLayout->addWidget(change); adminLayout->addStretch();
    connect(change,&QPushButton::clicked,&dialog,[&] {
        if(newPassword->text()!=confirm->text()) { QMessageBox::warning(&dialog,"修改失败","两次输入的新密码不一致。"); return; }
        QSettings settings; QString error;
        if(!AdminPassword::change(settings,current->text(),newPassword->text(),&error)) { QMessageBox::warning(&dialog,"修改失败",error); return; }
        current->clear(); newPassword->clear(); confirm->clear(); serverUnlocked=false; unlockedCredential.clear(); editor->setEnabled(false); unlock->setEnabled(true);
        lockState->setText("管理员密码已更新，服务器设置已重新锁定。");
        QMessageBox::information(&dialog,"修改成功","管理员密码已保存。后续解锁请使用新密码。");
    });
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel); buttons->setObjectName("softwareSettingsButtons");
    buttons->button(QDialogButtonBox::Save)->setText("保存并应用"); buttons->button(QDialogButtonBox::Cancel)->setText("取消"); outer->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        const QString folder=QDir::cleanPath(receiveFolder->text().trimmed());
        if(!QDir::isAbsolutePath(folder) || !QDir().mkpath(folder) || !QFileInfo(folder).isWritable()) {QMessageBox::warning(&dialog,"保存失败","请选择可写入的接收目录。");return;}
        QString startupError;
        if(autoStart->isChecked()!=SystemSettings::autoStartEnabled() && !SystemSettings::setAutoStart(autoStart->isChecked(),&startupError)) {QMessageBox::warning(&dialog,"保存失败",startupError);return;}
        QSettings().setValue("files/receiveDirectory",folder);
        RelayConfig chosen; chosen.host=host->text().trimmed(); chosen.port=quint16(port->value());
        chosen.fingerprint=RelayConfig::normalizePin(pin->text()); chosen.enrollmentKey=key->text(); QString error;
        const bool changed=chosen.host!=config.host || chosen.port!=config.port || chosen.fingerprint!=config.fingerprint || chosen.enrollmentKey!=config.enrollmentKey;
        if(changed) {
            if(!unlocked()) { QMessageBox::warning(&dialog,"服务器设置已锁定","请先输入管理员密码解锁服务器设置。"); return; }
            if (!chosen.valid(&error)) { QMessageBox::warning(&dialog,"配置无效",error); return; }
            QDir().mkpath(QFileInfo(configPath()).absolutePath()); QSaveFile file(configPath());
            if (!file.open(QIODevice::WriteOnly)) { QMessageBox::warning(&dialog,"保存失败","无法写入本机配置目录。"); return; }
            file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
            file.write(QJsonDocument(QJsonObject{{"server",chosen.host},{"port",chosen.port},{"fingerprint",chosen.fingerprint},{"enrollmentKey",chosen.enrollmentKey}}).toJson());
            if (!file.commit()) { QMessageBox::warning(&dialog,"保存失败","无法保存配置。"); return; }
        }
        const bool networkChanged=QSettings().value("network/forceRelay",false).toBool()!=forceRelay->isChecked();
        QSettings().setValue("network/forceRelay",forceRelay->isChecked());
        if((changed || networkChanged) && chosen.valid()) m_relay.start(chosen);
        dialog.accept();
        if(chosen.valid() && QSettings().value("shareEnabled",true).toBool() && !m_host.isListening())
            QTimer::singleShot(0,this,[this]{QString error;if(!startSharing(&error))message(error,true);});
    });
    dialog.exec();
}

QString MainWindow::passwordAccount(const QString &target) const {
    auto config=m_relay.config(); if (config.host.isEmpty()) config=readConfig();
    return "remote:"+config.host.toLower()+":"+QString::number(config.port)+":"+target;
}
