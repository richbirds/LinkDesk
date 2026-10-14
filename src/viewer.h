#pragma once
#include <QWidget>
#include <QMainWindow>
#include <QImage>
#include <QJsonObject>
#include <QSet>
#include <QElapsedTimer>
#include <QTimer>
#include "remote.h"
class RemoteClient;
class QLabel;
class QPushButton;
class RemoteCanvas : public QWidget {
    Q_OBJECT
public:
    explicit RemoteCanvas(QWidget *parent = nullptr);
    void setFrame(const QByteArray &jpeg);
    void setImage(const QImage &image);
    void setControl(bool enabled);
    void releaseInput();
    void clearFrame();
    bool controlEnabled() const { return m_control; }
signals:
    void input(const QJsonObject &event);
    void escapeRequested();
    void invalidFrame();
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void keyReleaseEvent(QKeyEvent *) override;
    void focusOutEvent(QFocusEvent *) override;
private:
    QRectF imageRect() const;
    void pointer(QMouseEvent *event, bool button, bool down);
    QImage m_image;
    bool m_control = false;
    QSet<int> m_keys;
    QElapsedTimer m_move;
};
class ViewerWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit ViewerWindow(RemoteClient *client, QWidget *parent = nullptr);
    void begin(const QString &name, bool control);
    void setManaged(bool managed);
    void preview();
signals:
    void expanded();
    void returnedToPreview();
protected:
    void closeEvent(QCloseEvent *) override;
    void changeEvent(QEvent *) override;
private:
    bool m_managed=false, m_preview=false;
    bool m_controlAllowed=false;
    StreamSettings m_fullSettings;
    RemoteClient *m_client;
    RemoteCanvas *m_canvas;
    QLabel *m_mode, *m_stats;
    QPushButton *m_leave;
    QElapsedTimer m_clock;
    QTimer m_statsTimer;
    int m_frames=0;
    qint64 m_bytes=0;
};
