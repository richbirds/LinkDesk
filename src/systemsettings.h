#pragma once
#include <QString>
namespace SystemSettings {
bool setAutoStart(bool enabled,QString *error);
bool autoStartEnabled();
bool power(const QString &action,QString *error);
}
