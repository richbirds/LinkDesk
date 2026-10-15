#include "singleinstance.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    const auto args=app.arguments();
    if(args.size()==3) {
        SingleInstance instance(args[2]);
        const auto result=instance.acquire([&] { QFile f(args[2]+"/activated");if(!f.open(QIODevice::WriteOnly)) app.exit(9); });
        if(result==SingleInstance::AlreadyRunning) return 42;
        if(result!=SingleInstance::Owner) return 2;
        if(args[1]=="--probe") return 0;
        QFile ready(args[2]+"/ready");if(!ready.open(QIODevice::WriteOnly)) return 3;ready.close();
        QTimer::singleShot(15000,&app,&QCoreApplication::quit);
        return app.exec();
    }
    QTemporaryDir state;if(!state.isValid()) return 4;
    QProcess owner;owner.start(app.applicationFilePath(),{"--owner",state.path()});
    if(!owner.waitForStarted(5000)) return 5;
    QElapsedTimer wait;wait.start();
    while(!QFile::exists(state.path()+"/ready") && wait.elapsed()<5000) QThread::msleep(20);
    auto stop=[&] {owner.kill();owner.waitForFinished(5000);};
    auto probe=[&](int expected) {
        QProcess child;child.start(app.applicationFilePath(),{"--probe",state.path()});
        const bool ok=child.waitForStarted(5000) && child.waitForFinished(5000) && child.exitStatus()==QProcess::NormalExit && child.exitCode()==expected;
        if(child.state()!=QProcess::NotRunning) {child.kill();child.waitForFinished(5000);}
        return ok;
    };
    if(!QFile::exists(state.path()+"/ready") || !probe(42)) {stop();return 6;}
    wait.restart();
    while(!QFile::exists(state.path()+"/activated") && wait.elapsed()<5000) QThread::msleep(20);
    const bool activated=QFile::exists(state.path()+"/activated");
    stop();
    if(!activated) return 7;
    // A killed owner must not leave an unusable identity lock/socket behind.
    if(!probe(0)) return 8;
    return 0;
}
