#pragma once
#include <QWidget>
#include <QList>
class RelayAgent;
class DeviceCard;
class QComboBox;
class QGridLayout;
class ConnectionList : public QWidget {
    Q_OBJECT
public:
    explicit ConnectionList(RelayAgent *agent,QWidget *parent=nullptr);
    void stopAll();
    void reconnectAll();
    bool connectDesktop(const QString &target,const QString &password,bool remember);
    bool wantsConnection(const QString &target) const;
    void disconnectDesktop(const QString &target);
signals:
    void connectionState(const QString &target,const QString &text,int state,bool wanted);
protected:
    void resizeEvent(QResizeEvent *) override;
private:
    RelayAgent *m_agent;
    QList<DeviceCard *> m_devices;
    QComboBox *m_groups;
    QGridLayout *m_grid;
    QWidget *m_gridWidget;
    QStringList m_groupNames;
    void edit(DeviceCard *device=nullptr);
    void save();
    void reflow();
    void refreshGroups();
    void manageGroup(bool remove);
    void bind(DeviceCard *device);
};
