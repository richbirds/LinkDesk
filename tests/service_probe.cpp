#include "remote.h"
#include "relay.h"
#include "platform.h"
#include "viewer.h"
#include <memory>
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
// Private integration descriptors are never part of public sources or logs.
int main(int argc,char **argv){QApplication app(argc,argv);app.setOrganizationName("LinkDeskTests");app.setApplicationName("ServiceProbe");Platform::initialize();const auto args=app.arguments();if(args.size()<3)return 2;
    QFile file(args[1]);if(!file.open(QIODevice::ReadOnly))return 3;const auto j=QJsonDocument::fromJson(file.readAll()).object();RelayConfig config{j.value("server").toString(),j.value("fingerprint").toString(),j.value("enrollmentKey").toString(),quint16(j.value("port").toInt(443))};QString reason;if(!config.valid(&reason))return 4;
    const auto id=j.value("id").toString(),password=j.value("password").toString();if(id.size()!=9)return 5;
    QTemporaryDir state;RelayAgent relay;RemoteClient client;QElapsedTimer clock;clock.start();int frames=0,resets=0,errors=0,lastPrintedReset=-1,lockPhase=0; qint64 lastFileLog=-1;bool active=false,saved=false,fileQueued=false;QTimer retry;
    const QString mode=args[2];std::unique_ptr<ViewerWindow> viewer;if(mode=="viewer"){viewer=std::make_unique<ViewerWindow>(&client);viewer->show();}QObject::connect(&retry,&QTimer::timeout,&app,[&]{if(!active && relay.online())relay.connectViaRelay(id);});retry.setInterval(2000);
    QObject::connect(&relay,&RelayAgent::onlineChanged,&app,[&](bool online,const QString &){if(online)relay.connectViaRelay(id);});
    QObject::connect(&relay,&RelayAgent::viewerReady,&app,[&](quint16 port,const QString &,const QString &pin){client.connectTo("127.0.0.1",port,mode=="wrong"?"invalid-machine-password":password,id,pin);});
    QObject::connect(&relay,&RelayAgent::error,&app,[&](const QString &){++errors;qInfo("Relay attempt failed; no credentials logged");if(mode!="watch" && mode!="viewer")app.exit(6);});
    QObject::connect(&client,&RemoteClient::error,&app,[&](const QString &){++errors;if(mode=="wrong"){app.exit(frames==0?0:7);return;}qInfo("Remote attempt failed; no credentials logged");if(mode!="watch" && mode!="viewer")app.exit(8);});
    QObject::connect(&client,&RemoteClient::disconnected,&app,[&]{active=false;relay.cancelViewer();if(mode=="watch" || mode=="viewer")retry.start();});
    QObject::connect(&client,&RemoteClient::connected,&app,[&](const QString &,bool control){active=true;retry.stop();if(viewer)viewer->begin("Windows 服务验收",control);qInfo("Authenticated service session; control=%d",control);if(mode=="files" && args.size()==4 && !fileQueued){fileQueued=true;client.sendFiles({args[3]});}});
    QObject::connect(&client,&RemoteClient::desktopReset,&app,[&]{++resets;qInfo("Desktop boundary cleared decoder and image; resets=%d",resets);});
    QObject::connect(&client,&RemoteClient::imageFrame,&app,[&](const QImage &image,qint64,bool){++frames;if(mode=="watch" && args.size()==4)image.save(args[3]);if(frames==1||frames%30==0||lastPrintedReset!=resets){lastPrintedReset=resets;qInfo("Service frame: %dx%d frames=%d resets=%d elapsed=%lld ms",image.width(),image.height(),frames,resets,clock.elapsed());}
        if(mode=="sas" && args.size()==4)image.save(args[3]);
        if(mode=="sas" && frames==1){client.requestSecureAttention();qInfo("Requested operating-system SAS through authenticated service capability.");QTimer::singleShot(30000,&app,[&]{qInfo("SAS observation: active=%d frames=%d resets=%d",client.active(),frames,resets);app.exit(client.active() && frames>1 && resets>0?0:15);});}
        if(mode=="held-key" && frames==1){client.sendInput({{"action","key"},{"key",int(Qt::Key_Shift)},{"down",true}});qInfo("Held synthetic test modifier; waiting for worker recovery.");QTimer::singleShot(90000,&app,[&]{client.sendInput({{"action","key"},{"key",int(Qt::Key_Shift)},{"down",false}});app.exit(0);});}
        if(mode=="lock" && frames==1){lockPhase=1;client.sendInput({{"action","key"},{"key",int(Qt::Key_Meta)},{"down",true}});client.sendInput({{"action","key"},{"key",int(Qt::Key_L)},{"down",true}});client.sendInput({{"action","key"},{"key",int(Qt::Key_L)},{"down",false}});client.sendInput({{"action","key"},{"key",int(Qt::Key_Meta)},{"down",false}});qInfo("Sent lock shortcut through authenticated remote input.");}
        if(mode=="lock" && lockPhase==1 && resets>0){lockPhase=2;if(args.size()==4)image.save(args[3]);QTimer::singleShot(500,&app,[&]{client.sendInput({{"action","move"},{"x",.2},{"y",.2}});client.sendInput({{"action","key"},{"key",int(Qt::Key_Tab)},{"down",true}});client.sendInput({{"action","key"},{"key",int(Qt::Key_Tab)},{"down",false}});});QTimer::singleShot(6000,&app,[&]{qInfo("Lock boundary frame restored; transport after input active=%d",client.active());app.exit(client.active() && frames>1?0:14);});}
        if(mode=="snapshot" && args.size()==4 && !saved){saved=image.save(args[3]);if(!saved)app.exit(9);QTimer::singleShot(1000,&app,[&]{app.exit(client.active()?0:10);});}
        if((mode=="input" || mode=="login-input" || mode=="escape-input" || mode=="click-input") && frames==1){client.sendInput({{"action","move"},{"x",.5},{"y",.5}});
            if(mode=="login-input" || mode=="escape-input"){const int key=mode=="login-input"?Qt::Key_Space:Qt::Key_Escape;client.sendInput({{"action","key"},{"key",key},{"down",true}});client.sendInput({{"action","key"},{"key",key},{"down",false}});}
            if(mode=="click-input"){client.sendInput({{"action","button"},{"x",.555},{"y",.638},{"button",1},{"down",true}});client.sendInput({{"action","button"},{"x",.555},{"y",.638},{"button",1},{"down",false}});}
            QTimer::singleShot(10000,&app,[&]{qInfo("Input probe: active=%d frames=%d",client.active(),frames);app.exit(client.active() && (mode=="input" || frames>1)?0:11);});}
        if((mode=="login-input" || mode=="escape-input" || mode=="click-input") && frames>1 && args.size()==4)image.save(args[3]);
    });
    QObject::connect(&client,&RemoteClient::powerResult,&app,[](const QString &message){qInfo().noquote()<<message;});
    QObject::connect(&client,&RemoteClient::fileProgress,&app,[&](const QString &,qint64 done,qint64 total,const QString &state){if(done>=lastFileLog+1024*1024 || (lastFileLog<0 && done>0)){lastFileLog=done;qInfo("File acknowledged progress: %lld / %lld bytes",done,total);}if(state=="传送完成"&&done==total){qInfo("User-broker file transfer acknowledged: %lld bytes",total);app.exit(0);}else if(state.startsWith("传送失败"))app.exit(12);});
    QTimer deadline;deadline.setSingleShot(true);QObject::connect(&deadline,&QTimer::timeout,&app,[&]{qInfo("Probe summary: frames=%d resets=%d errors=%d active=%d",frames,resets,errors,active);app.exit((mode=="watch" || mode=="viewer") && frames>0?0:13);});deadline.start((mode=="watch" || mode=="viewer")?600000:mode=="files"?240000:mode=="held-key"?120000:60000);
    relay.start(config,state.filePath("identity.json"));return app.exec();}
