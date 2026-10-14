#include "remote.h"
#include "platform.h"
#include "syntheticdesktop.h"
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QCryptographicHash>
int main(int argc,char **argv) {
#ifdef Q_OS_WIN
    if(qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR",(qEnvironmentVariable("WINDIR")+"/Fonts").toUtf8());
#endif
    QApplication app(argc,argv);app.setOrganizationName("LinkDeskBenchmarks");app.setApplicationName("LinkDeskSyntheticBenchmark");QStandardPaths::setTestModeEnabled(true);Platform::initialize();
    const auto args=app.arguments();const int index=args.indexOf("--frames");const int frames=index>=0 && index+1<args.size() ? qBound(30,args[index+1].toInt(),300) : 60;
    QJsonArray rows;
    for(const auto &mode:QStringList{"baseline","office","auto"}) for(const auto &scene:QStringList{"static","typing","scroll","drag","video"}) {
        // Prepare identical inputs before starting either TLS connection or timers.
        QList<QImage> images;for(int n=0;n<frames;++n) images.append(syntheticDesktop(scene,n));
        QCryptographicHash digest(QCryptographicHash::Sha256);for(const auto &image:images) digest.addData(QByteArrayView(reinterpret_cast<const char *>(image.constBits()),image.sizeInBytes()));
        RemoteHost host;RemoteClient client;QString error;QJsonObject latest;
        client.setProperty("legacyOffice",mode=="baseline");client.setProperty("disableVideo",mode!="auto");
        StreamSettings settings;settings.width=1280;settings.height=720;settings.fps=30;settings.quality=88;settings.codec=mode=="auto" ? "auto" : "desktop";client.setStreamSettings(settings);
        if(!host.start(0,&error,true,QHostAddress::LocalHost)) { QTextStream(stderr)<<error;return 1; }host.setSyntheticFrame(images.first());
        bool connected=false;QEventLoop setup;QTimer deadline;deadline.setSingleShot(true);
        QObject::connect(&client,&RemoteClient::connected,&setup,[&]{connected=true;setup.quit();});QObject::connect(&client,&RemoteClient::error,&setup,[&](const QString &e){error=e;setup.quit();});QObject::connect(&deadline,&QTimer::timeout,&setup,&QEventLoop::quit);
        deadline.start(10000);client.connectTo("127.0.0.1",host.port(),host.code(),"synthetic",host.fingerprint());setup.exec();if(!connected) { QTextStream(stderr)<<"TLS setup failed: "<<error;return 1; }
        QEventLoop run;QTimer input;input.setTimerType(Qt::PreciseTimer);input.setInterval(33);int next=0,received=0;QElapsedTimer clock;clock.start();
        QObject::connect(&client,&RemoteClient::imageFrame,&run,[&](const QImage &,qint64,bool changed){if(changed) ++received;});
        QObject::connect(&input,&QTimer::timeout,&run,[&]{if(next<images.size()) host.setSyntheticFrame(images[next++]);else { input.stop();QTimer::singleShot(700,&run,&QEventLoop::quit); }});
        input.start();run.exec();auto metrics=host.streamingMetrics();metrics["mode"]=mode;metrics["scene"]=scene;metrics["inputFrames"]=frames;metrics["decodedFrames"]=received;metrics["elapsedMs"]=double(clock.elapsed());metrics["size"]="1280x720";metrics["quality"]=88;metrics["network"]="loopback TLS/TCP";
        metrics["inputDigest"]=QString::fromLatin1(digest.result().toHex());
        rows.append(metrics);QTextStream(stdout)<<QJsonDocument(metrics).toJson(QJsonDocument::Compact)<<Qt::endl;client.disconnectPeer();host.stop();
    }
    const int output=args.indexOf("--output");if(output>=0 && output+1<args.size()) { QFile file(args[output+1]);if(!file.open(QIODevice::WriteOnly)) return 1;file.write(QJsonDocument(rows).toJson()); }
    return 0;
}
