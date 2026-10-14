#pragma once
#include <QImage>
#include <QRect>
#include <QString>
class QScreen;
namespace Platform {
void initialize();
bool savePassword(const QString &account,const QString &password);
QString loadPassword(const QString &account);
bool removePassword(const QString &account);
QString bundledConfigPath();
bool generateIdentity(const QString &certPath,const QString &keyPath,QString *error);
bool screenCaptureAvailable(QString *error);
QRect screenGeometry(QScreen *screen);
QImage captureScreen(QScreen *screen, const QSize &maximum=QSize(1920,1080));
QString permissionHelp();
}
