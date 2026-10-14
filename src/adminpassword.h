#pragma once
#include <QString>
class QSettings;
namespace AdminPassword {
void initialize(QSettings &settings);
bool verify(QSettings &settings,const QString &password);
bool change(QSettings &settings,const QString &current,const QString &next,QString *error=nullptr);
}
