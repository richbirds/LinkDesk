// Manual cross-platform TLS file/resume probe. Keep runtime descriptors private.
#include "remote.h"
#include "platform.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QTemporaryDir>
int main(int argc,char **argv) {
    QApplication app(argc,argv);app.setOrganizationName("LinkDeskTests");app.setApplicationName("FileProbe");
    QTemporaryDir profile;QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,profile.path());
    Platform::initialize();const auto args=app.arguments();if(args.size()<4)return 2;
    QTimer::singleShot(120000,&app,[&]{app.exit(1);});RemoteHost host;RemoteClient client;
    if(args[1]=="host") {
        QSettings().setValue("files/receiveDirectory",args[3]);QString error;if(!host.start(0,&error,true))return 3;
        QSaveFile descriptor(args[2]);if(!descriptor.open(QIODevice::WriteOnly))return 4;
        descriptor.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        descriptor.write(QJsonDocument(QJsonObject{{"port",int(host.port())},{"password",host.code()},{"fingerprint",host.fingerprint()}}).toJson());if(!descriptor.commit())return 5;
        QObject::connect(&host,&RemoteHost::fileReceived,&app,[&](const QString &path){
            QFile file(path);if(!file.open(QIODevice::ReadOnly)){app.exit(6);return;}QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(&file);
            QSaveFile result(args[2]+".result");if(!result.open(QIODevice::WriteOnly)){app.exit(7);return;}result.write(hash.result().toHex());result.commit();
            QTimer::singleShot(500,&app,&QCoreApplication::quit);
        });
    } else if(args[1]=="send" && args.size()==5) {
        QFile file(args[2]);if(!file.open(QIODevice::ReadOnly))return 4;
        const auto descriptor=QJsonDocument::fromJson(file.readAll()).object();
        auto connectPeer=[&]{client.connectTo(args[3],quint16(descriptor["port"].toInt()),descriptor["password"].toString(),"file-probe",descriptor["fingerprint"].toString());};
        bool queued=false,interrupted=false;
        QObject::connect(&client,&RemoteClient::connected,&app,[&](const QString &,bool){if(!queued){queued=true;client.sendFiles({args[4]});}});
        QObject::connect(&client,&RemoteClient::fileProgress,&app,[&](const QString &,qint64 done,qint64,const QString &state){
            if(state=="传送中" && done>=262144 && !interrupted){interrupted=true;client.disconnectPeer();QTimer::singleShot(500,&app,connectPeer);}
            if(state=="传送完成")app.exit(interrupted ? 0 : 8);
            if(state.startsWith("传送失败")){qCritical().noquote()<<state;app.exit(9);}
        });connectPeer();return app.exec();
    } else return 2;
    return app.exec();
}
