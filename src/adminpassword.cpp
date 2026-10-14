#include "adminpassword.h"
#include <QSettings>
#include <QPasswordDigestor>
#include <QCryptographicHash>
#include <QRandomGenerator>
static constexpr int Iterations=100000;
static QByteArray digest(const QString &password,const QByteArray &salt) {
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,password.toUtf8(),salt,Iterations,32);
}
static bool store(QSettings &settings,const QString &password) {
    QByteArray salt(16,Qt::Uninitialized);
    for(auto &byte:salt) byte=char(QRandomGenerator::system()->generate()&255);
    settings.setValue("administrator/credential",QString::fromLatin1(salt.toHex())+":"+QString::fromLatin1(digest(password,salt).toHex()));
    settings.sync(); return settings.status()==QSettings::NoError;
}
void AdminPassword::initialize(QSettings &settings) {
    if(!settings.contains("administrator/credential")) store(settings,"888888");
}
bool AdminPassword::verify(QSettings &settings,const QString &password) {
    initialize(settings);
    const auto pieces=settings.value("administrator/credential").toString().split(':');
    if(pieces.size()!=2 || pieces[0].size()!=32 || pieces[1].size()!=64) return false;
    const auto salt=QByteArray::fromHex(pieces[0].toLatin1()),expected=QByteArray::fromHex(pieces[1].toLatin1());
    if(salt.size()!=16 || expected.size()!=32) return false;
    const auto actual=digest(password,salt); unsigned int diff=0;
    for(int i=0;i<32;++i) diff|=quint8(actual[i])^quint8(expected[i]);
    return diff==0;
}
bool AdminPassword::change(QSettings &settings,const QString &current,const QString &next,QString *error) {
    if(!verify(settings,current)) { if(error) *error="当前管理员密码不正确。"; return false; }
    if(next.size()<6 || next.size()>64 || next.contains(QChar::Null) || next.contains('\n') || next.contains('\r')) {
        if(error) *error="新管理员密码需为 6–64 个字符，不能包含换行。"; return false;
    }
    if(!store(settings,next)) { if(error) *error="无法保存管理员密码。"; return false; }
    return true;
}
