#pragma once
#include <QMainWindow>
#include <QPointer>
#include "remote.h"
#include "relay.h"
class QSystemTrayIcon;
class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QSpinBox;
class ViewerWindow;
class ConnectionList;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent=nullptr);
    ~MainWindow() override;
public slots:
    void requestExit();
    void restoreFromTray();
protected:
    bool eventFilter(QObject *,QEvent *) override;
protected:
    void closeEvent(QCloseEvent *) override;
private:
#ifdef Q_OS_WIN
    bool m_serviceManaged=false,m_servicePolling=false;
    QTimer m_serviceWatch;
    QString m_machineId;
    quint64 m_serviceFileSequence=0;
    void pollMachineService();
    void configureMachineService();
#endif
    QSystemTrayIcon *m_tray=nullptr;
    bool m_exiting=false,m_confirmingExit=false,m_inTray=false;
    Qt::WindowStates m_savedState;
    QList<QPointer<QWidget>> m_hiddenWindows;
    void hideToTray();
    void beginExit();
    QTimer m_shareRetry;
    void restoreSharing();
    RemoteHost m_host;
    RelayAgent m_relay;
    ConnectionList *m_list;
    QLabel *m_address,*m_code,*m_hostStatus,*m_banner,*m_peer,*m_fingerprint;
    QLineEdit *m_target,*m_password;
    QLabel *m_server, *m_networkState;
    QPushButton *m_share,*m_connect,*m_disconnectHost;
    QCheckBox *m_control,*m_rememberPassword,*m_saveHostPassword;
    QString m_hostViewer, m_connectingTarget;
    QString passwordAccount(const QString &target) const;
    void toggleShare();
    bool startSharing(QString *error);
    void connectRemote();
    void message(const QString &text, bool error=false);
    void configureServer();
    void startConfiguredServer();
};
