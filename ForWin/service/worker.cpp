#include "native.h"
#include "pipe.h"
#include "platform.h"
#include "input.h"
#include "protocol.h"
#include "filetransfer.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTimer>
#include <QSettings>
#include <QBuffer>
#include <QElapsedTimer>
#include <QtEndian>
#include <algorithm>
#include <thread>
using namespace HostService;
static QByteArray json(const QJsonObject &j){return QJsonDocument(j).toJson(QJsonDocument::Compact);}
struct DesktopHandle {
    HDESK value=nullptr;DesktopHandle()=default;explicit DesktopHandle(HDESK d):value(d){}
    ~DesktopHandle(){if(value)CloseDesktop(value);}
    DesktopHandle(const DesktopHandle &)=delete;DesktopHandle &operator=(const DesktopHandle &)=delete;
    DesktopHandle(DesktopHandle &&d) noexcept:value(d.value){d.value=nullptr;}
    DesktopHandle &operator=(DesktopHandle &&d) noexcept{if(value)CloseDesktop(value);value=d.value;d.value=nullptr;return *this;}
    explicit operator bool() const{return value!=nullptr;}
};
static QString desktopName(HDESK d){WCHAR name[256]{};DWORD n=0;return d && GetUserObjectInformationW(d,UOI_NAME,name,sizeof(name),&n)?QString::fromWCharArray(name):QString();}
static bool fileType(const QJsonObject &j){const auto t=j.value("type").toString();return t.startsWith("file_") && t.size()<32;}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);app.setOrganizationName("LinkDesk");app.setApplicationName("LinkDesk");
    const auto args=app.arguments();
    if(args.size()!=5 || (args[1]!="--capture" && args[1]!="--user-broker"))return 2;
    const bool capture=args[1]=="--capture";bool numeric=false;const DWORD parent=args[3].toULong(&numeric);
    if(!numeric || !parent || parent!=servicePid() || !args[2].startsWith(capture?"LinkDeskHost-worker-":"LinkDeskHost-user-"))return 3;
    if(capture!=systemProcess(GetCurrentProcessId()))return 4;
    QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath()});
    const bool secure=args[4]=="secure";if(args[4]!="secure" && args[4]!="ordinary")return 5;
    auto handle=clientPipe(args[2],parent);if(!handle)return 6;
    std::atomic<bool> active{false},rescan{true},failed{false};std::mutex mutex;
    QSize maximum(480,270);int fps=1;quint64 epoch=0;
    std::deque<QJsonObject> inputs;
    std::unique_ptr<Wire> broker;std::unique_ptr<FileTransfer> files;
    std::unique_ptr<PipeChannel> pipe;
    pipe=std::make_unique<PipeChannel>(std::move(handle),false,std::function<bool(HANDLE)>(),[&](Kind k,const QByteArray &b){
        const auto j=(k==Kind::Command || k==Kind::FileJson)?QJsonDocument::fromJson(b).object():QJsonObject();
        if(capture){
            if(k!=Kind::Command || j.isEmpty()){failed=true;return;}
            const auto type=j.value("type").toString();
            if(type=="settings"){
                std::lock_guard<std::mutex> lock(mutex);const int w=j.value("width").toInt(),h=j.value("height").toInt(),f=j.value("fps").toInt();
                if(w<160||w>4096||h<90||h>4096||qint64(w)*h>8000000||f<1||f>30){failed=true;return;}
                maximum=QSize(w,h);fps=f;active=j.value("active").toBool();
                if(!active){inputs.erase(std::remove_if(inputs.begin(),inputs.end(),[](const QJsonObject &e){return e.value("type")!="release_known";}),inputs.end());inputs.push_back({{"action","release"}});}
            }else if(type=="rescan")rescan=true;
            else if(type=="input" || type=="release_known"){
                std::lock_guard<std::mutex> lock(mutex);if(inputs.size()>=256){inputs.clear();inputs.push_back({{"action","release"}});return;}
                if(type=="input"){if(!active || quint64(j.value("epoch").toDouble())!=epoch)return;auto event=j.value("event").toObject();inputs.push_back(event);}
                else inputs.push_back(j);
            }else failed=true;
        }else{
            if(k!=Kind::FileJson && k!=Kind::FileChunk){failed=true;return;}
            if(k==Kind::FileJson && !fileType(j)){failed=true;return;}
            QMetaObject::invokeMethod(&app,[&,k,b]{if(!broker)return;broker->feed(Wire::packet(k==Kind::FileJson?Wire::Json:Wire::File,b));},Qt::QueuedConnection);
        }
    },[&]{failed=true;});
    const auto launchDesktop=desktopName(GetThreadDesktop(GetCurrentThreadId()));
    std::thread thread;
    if(!capture){
        broker=std::make_unique<Wire>(std::function<bool(Wire::Type,const QByteArray &)>([&](Wire::Type k,const QByteArray &b){
            if(k==Wire::Json && fileType(QJsonDocument::fromJson(b).object()))return pipe->send(Kind::FileJson,b);
            if(k==Wire::File)return pipe->send(Kind::FileChunk,b);return false;
        }));
        files=std::make_unique<FileTransfer>();files->attach(broker.get());
        QObject::connect(files.get(),&FileTransfer::received,&app,[&](const QString &path){pipe->send(Kind::Event,json({{"type","file_complete"},{"path",path}}));});
        pipe->send(Kind::Event,json({{"type","user_ready"}}));
    }else thread=std::thread([&]{
        // This thread owns every desktop-bound resource. QCoreApplication and
        // its pipe/event loop never create a window or hook on this thread.
        const auto previousDpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        InputController input;const auto original=GetThreadDesktop(GetCurrentThreadId());DesktopHandle bound;QString boundName;QElapsedTimer cadence;cadence.start();qint64 last=-10000,lastWait=-10000;QByteArray oldCursor;QString requestedDesktop;
        auto boundary=[&]{input.releaseAll();Platform::stopCapture();oldCursor.clear();
            {std::lock_guard<std::mutex> lock(mutex);++epoch;inputs.erase(std::remove_if(inputs.begin(),inputs.end(),[](const QJsonObject &e){return e.value("type")!="release_known";}),inputs.end());}
            pipe->send(Kind::Event,json({{"type","boundary"},{"epoch",double(epoch)}}));
        };
        while(!failed && pipe->live()){
            DesktopHandle target(OpenInputDesktop(0,FALSE,GENERIC_ALL));
            const auto name=desktopName(static_cast<HDESK>(target.value));
            if(!target || name.isEmpty() || (!secure && name.compare("Default",Qt::CaseInsensitive)!=0)){
                if(!boundName.isEmpty()){boundary();boundName.clear();}
                if(cadence.elapsed()-lastWait>=1000){lastWait=cadence.elapsed();pipe->send(Kind::Event,json({{"type","wait"},{"message","输入桌面暂不可访问，等待会话恢复"}}));}
                Sleep(50);continue;
            }
            // SendInput access is established when the process joins its startup
            // desktop. A thread rebind alone can capture Winlogon yet be denied
            // input. Ask the supervisor to replace this fixed worker in that
            // desktop; never move the GUI or grant it SYSTEM rights.
            if(name.compare(launchDesktop,Qt::CaseInsensitive)!=0){
                if(requestedDesktop!=name){boundary();requestedDesktop=name;pipe->send(Kind::Event,json({{"type","desktop_rebind"},{"epoch",double(epoch)},{"desktop",name}}));}
                Sleep(50);continue;
            }
            if(name!=boundName || rescan.exchange(false)){
                boundary();
                if(!SetThreadDesktop(static_cast<HDESK>(target.value))){pipe->send(Kind::Event,json({{"type","wait"},{"message",winError("绑定输入桌面")}}));Sleep(100);continue;}
                bound=std::move(target);boundName=name;last=-10000;
                pipe->send(Kind::Event,json({{"type","desktop_ready"},{"epoch",double(epoch)},{"desktop",boundName}}));
            }
            std::deque<QJsonObject> work;QSize size;int rate;
            {std::lock_guard<std::mutex> lock(mutex);work.swap(inputs);size=maximum;rate=fps;}
            const auto geometry=Platform::screenGeometry(nullptr);
            for(const auto &event:work){
                if(event.value("type")=="release_known"){
                    QSet<int> keys,buttons;for(auto k:event.value("keys").toArray())if(keys.size()<256 && InputController::nativeKeySupported(k.toInt()))keys.insert(k.toInt());for(auto b:event.value("buttons").toArray())if(b.toInt()>=1 && b.toInt()<=3)buttons.insert(b.toInt());
                    InputController::clearNativeError();input.releaseKnown(keys,buttons);
                    pipe->send(Kind::Event,json({{"type","release_applied"},{"epoch",double(epoch)},{"serial",event.value("serial")},{"error",double(InputController::nativeError())}}));
                }else {InputController::clearNativeError();input.apply(event,geometry);
                    pipe->send(Kind::Event,json({{"type","input_result"},{"epoch",double(epoch)},{"error",double(InputController::nativeError())}}));}
            }
            if(active && cadence.elapsed()-last>=qMax(1,1000/rate)){
                last=cadence.elapsed();const auto frame=Platform::captureFrame(nullptr,size);
                if(frame.image.isNull()){
                    if(last-lastWait>=1000){lastWait=last;pipe->send(Kind::Event,json({{"type","wait"},{"message",frame.waitReason}}));}
                }else{
                    const auto image=frame.image.convertToFormat(QImage::Format_RGB32);
                    if(image.width()>0 && image.height()>0 && qint64(image.width())*image.height()<=8000000){
                        QByteArray bytes(32,Qt::Uninitialized);qToBigEndian<quint64>(epoch,bytes.data());
                        const qint32 fields[]{geometry.x(),geometry.y(),geometry.width(),geometry.height(),image.width(),image.height()};
                        for(int i=0;i<6;++i)qToBigEndian<quint32>(quint32(fields[i]),bytes.data()+8+i*4);
                        bytes.append(reinterpret_cast<const char *>(image.constBits()),image.width()*image.height()*4);pipe->send(Kind::Image,bytes);
                    }
                    auto cursor=Platform::captureCursor(geometry);QByteArray shape;QBuffer buffer(&shape);buffer.open(QIODevice::WriteOnly);if(!cursor.image.isNull())cursor.image.save(&buffer,"PNG");
                    auto event=json({{"type","cursor"},{"epoch",double(epoch)},{"x",cursor.position.x()},{"y",cursor.position.y()},{"visible",cursor.visible},{"hx",cursor.hotspot.x()},{"hy",cursor.hotspot.y()},{"image",QString::fromLatin1(shape.toBase64())}});
                    if(event!=oldCursor && event.size()<65536){pipe->send(Kind::Event,event);oldCursor=event;}
                }
            }
            Sleep(5);
        }
        input.releaseAll();Platform::stopCapture();
        SetThreadDesktop(original);if(previousDpi)SetThreadDpiAwarenessContext(previousDpi);
    });
    QTimer watch;watch.setInterval(100);QObject::connect(&watch,&QTimer::timeout,&app,[&]{if(failed || !pipe->live())app.quit();});watch.start();
    const int result=app.exec();failed=true;active=false;if(thread.joinable())thread.join();if(files)files->shutdown();pipe->stop();pipe.reset();return result;
}
