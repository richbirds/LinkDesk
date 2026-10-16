#pragma once
#include "native.h"
#include <QString>
#include <QJsonObject>
class QWidget;
namespace HostService {
// Ordinary-user request exporter. Only the elevated setup tool imports it.
bool launchSetup(const Config &request,const QJsonObject &migration,bool reconfigure,QString *error);
bool launchRemoval(QString *error);
}
