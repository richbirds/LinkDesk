#include "native.h"
#include "platform.h"
#include "relay.h"
#include <QRandomGenerator>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QThread>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSslCertificate>
#include <QSslKey>
#include <aclapi.h>
#include <sddl.h>
#include <QSet>
using namespace HostService;
static bool stopService(QString *error){
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm){*error=winError("Open service manager");return false;}
    SC_HANDLE svc=OpenServiceW(scm,Name,SERVICE_STOP|SERVICE_QUERY_STATUS);if(!svc){auto code=GetLastError();CloseServiceHandle(scm);return code==ERROR_SERVICE_DOES_NOT_EXIST;}
    SERVICE_STATUS_PROCESS initial{};DWORD initialBytes=0;Handle process;
    if(QueryServiceStatusEx(svc,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE *>(&initial),sizeof(initial),&initialBytes) && initial.dwProcessId)process.reset(OpenProcess(SYNCHRONIZE,FALSE,initial.dwProcessId));
    SERVICE_STATUS s{};ControlService(svc,SERVICE_CONTROL_STOP,&s);QElapsedTimer timer;timer.start();bool stopped=false;
    while(timer.elapsed()<15000){SERVICE_STATUS_PROCESS p{};DWORD n=0;if(!QueryServiceStatusEx(svc,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE *>(&p),sizeof(p),&n))break;if(p.dwCurrentState==SERVICE_STOPPED){stopped=true;break;}QThread::msleep(100);}
    if(stopped && process && WaitForSingleObject(process.value,DWORD(qMax<qint64>(1,15000-timer.elapsed())))!=WAIT_OBJECT_0)stopped=false;
    CloseServiceHandle(svc);CloseServiceHandle(scm);if(!stopped)*error="Windows service did not stop; no files were replaced";return stopped;
}
static bool startService(QString *error){SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm)return false;SC_HANDLE svc=OpenServiceW(scm,Name,SERVICE_START|SERVICE_QUERY_STATUS);bool ok=svc && (StartServiceW(svc,0,nullptr)||GetLastError()==ERROR_SERVICE_ALREADY_RUNNING);
    if(ok){ok=false;QElapsedTimer timer;timer.start();while(timer.elapsed()<15000){SERVICE_STATUS_PROCESS state{};DWORD n=0;if(!QueryServiceStatusEx(svc,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE *>(&state),sizeof(state),&n))break;if(state.dwCurrentState==SERVICE_RUNNING){ok=true;break;}if(state.dwCurrentState==SERVICE_STOPPED){*error=winError("Service initialization",state.dwWin32ExitCode);break;}QThread::msleep(100);}}
    if(!ok && error->isEmpty())*error=winError("Start Windows service");if(svc)CloseServiceHandle(svc);CloseServiceHandle(scm);return ok;}
static bool removeService(QString *error){if(!stopService(error))return false;SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);if(!scm)return false;SC_HANDLE svc=OpenServiceW(scm,Name,DELETE);bool ok=svc?DeleteService(svc):GetLastError()==ERROR_SERVICE_DOES_NOT_EXIST;if(!ok)*error=winError("Remove Windows service");if(svc)CloseServiceHandle(svc);CloseServiceHandle(scm);return ok;}
static bool secureFile(const QString &path,bool directory,QString *error){
    const QString sddl=directory?"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)":"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&sd,nullptr))return false;
    const auto pathW=QDir::toNativeSeparators(path).toStdWString();const bool ok=SetFileSecurityW(pathW.c_str(),OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,sd);LocalFree(sd);if(!ok)*error=winError("Protect installed component");return ok;
}
static bool copyBundle(const QString &destination,QString *error,const QString &source=QCoreApplication::applicationDirPath()){
    if(!protectDirectory(destination,true,error))return false;
    const QSet<QString> executables={"LinkDeskService.exe","LinkDeskWorker.exe","LinkDeskServiceSetup.exe"};
    const QSet<QString> libraries={"Qt6Core.dll","Qt6Gui.dll","Qt6Widgets.dll","Qt6Network.dll","Qt6Concurrent.dll","Qt6Svg.dll","libgcc_s_seh-1.dll","libstdc++-6.dll","libwinpthread-1.dll"};
    const QSet<QString> folders={"platforms","tls","imageformats","iconengines","styles","networkinformation","generic"};
    QStringList files;for(auto name:executables){if(!QFile::exists(source+"/"+name)){*error="Missing service bundle component: "+name;return false;}files<<name;}
    for(auto name:libraries)if(QFile::exists(source+"/"+name))files<<name;
    for(auto folder:folders){QDirIterator it(source+"/"+folder,QDir::Files,QDirIterator::Subdirectories);while(it.hasNext()){const auto path=it.next();if(!noReparse(path)){*error="Reparse point in bundle";return false;}if(path.endsWith(".dll",Qt::CaseInsensitive))files<<QDir(source).relativeFilePath(path);}}
    for(const auto &name:files){const auto from=source+"/"+name,to=destination+"/"+name;if(!noReparse(from)||!noReparse(to)){*error="Unsafe bundle path";return false;}
        if(!QDir().mkpath(QFileInfo(to).absolutePath()))return false;
        if(QFileInfo(from).absoluteFilePath().compare(QFileInfo(to).absoluteFilePath(),Qt::CaseInsensitive)!=0){QFile input(from);if(!input.open(QIODevice::ReadOnly)){*error="Cannot read bundle component";return false;}QSaveFile output(to);if(!output.open(QIODevice::WriteOnly)){*error="Cannot replace installed component; close old setup windows";return false;}while(!input.atEnd()){const auto b=input.read(256*1024);if(b.isEmpty()&&input.error()!=QFileDevice::NoError)return false;if(output.write(b)!=b.size())return false;}if(!output.commit())return false;}
        if(!secureFile(to,false,error))return false;
    }
    QDirIterator dirs(destination,QDir::Dirs|QDir::NoDotAndDotDot,QDirIterator::Subdirectories);while(dirs.hasNext()){const auto dir=dirs.next();if(!noReparse(dir)||!secureFile(dir,true,error))return false;}
    return true;
}
static bool registerService(QString *error){
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT|SC_MANAGER_CREATE_SERVICE);if(!scm){*error=winError("Open service manager");return false;}
    auto path=("\""+QDir::toNativeSeparators(installDirectory()+"/LinkDeskService.exe")+"\"").toStdWString();
    SC_HANDLE svc=OpenServiceW(scm,Name,SERVICE_CHANGE_CONFIG|SERVICE_QUERY_CONFIG|SERVICE_START);
    bool ok=false;
    if(svc)ok=ChangeServiceConfigW(svc,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,path.c_str(),nullptr,nullptr,nullptr,L"LocalSystem",nullptr,L"LinkDesk unattended host");
    else {svc=CreateServiceW(scm,Name,L"LinkDesk unattended host",SERVICE_CHANGE_CONFIG|SERVICE_QUERY_CONFIG|SERVICE_START,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,path.c_str(),nullptr,nullptr,nullptr,L"LocalSystem",nullptr);ok=svc;}
    if(ok){wchar_t description[]=L"Authenticated LinkDesk machine host with isolated interactive workers.";SERVICE_DESCRIPTIONW d{description};ok=ChangeServiceConfig2W(svc,SERVICE_CONFIG_DESCRIPTION,&d);SC_ACTION actions[]{{SC_ACTION_RESTART,5000},{SC_ACTION_RESTART,15000},{SC_ACTION_RESTART,60000}};SERVICE_FAILURE_ACTIONSW recovery{};recovery.dwResetPeriod=86400;recovery.cActions=3;recovery.lpsaActions=actions;if(ok)ok=ChangeServiceConfig2W(svc,SERVICE_CONFIG_FAILURE_ACTIONS,&recovery);}
    if(!ok)*error=winError("Register Windows service");if(svc)CloseServiceHandle(svc);CloseServiceHandle(scm);return ok;
}
static bool apply(const Config &config,const QJsonObject &migration,QString *error){
    const auto destination=installDirectory(),stage=destination+".staging",backup=destination+".rollback";
    const bool replacing=QFileInfo(QCoreApplication::applicationDirPath()).absoluteFilePath().compare(QFileInfo(destination).absoluteFilePath(),Qt::CaseInsensitive)!=0;
    const bool existed=installed();const auto marker=dataDirectory()+"/installation-incomplete";
    if(QFile::exists(marker)){*error="An interrupted installation needs administrator recovery before starting the service";return false;}
    if(!protectDirectory(QFileInfo(dataDirectory()).absolutePath(),false,error)||!protectDirectory(dataDirectory(),false,error))return false;
    // Stage the complete allowlisted bundle before touching the running service.
    if(replacing){
        for(const auto &path:{stage,backup})if(QFileInfo::exists(path)){if(!trustedPath(path,error))return false;*error="An earlier installation transaction needs administrator review: "+path;return false;}
        if(!copyBundle(stage,error)){QDir(stage).removeRecursively();return false;}
    }
    const auto policyPath=dataDirectory()+"/machine.dat",journalPath=dataDirectory()+"/migration.json";
    QByteArray oldPolicy;const bool hadPolicy=QFile::exists(policyPath),hadJournal=QFile::exists(journalPath);
    if(hadPolicy){if(!trustedPath(policyPath,error,true))return false;QFile file(policyPath);if(!file.open(QIODevice::ReadOnly))return false;oldPolicy=file.readAll();}
    if(hadJournal){QFile file(journalPath);if(!trustedPath(journalPath,error,true)||!file.open(QIODevice::ReadOnly)||file.size()>8192){if(replacing)QDir(stage).removeRecursively();return false;}const auto pending=QJsonDocument::fromJson(file.readAll()).object();if(pending.value("server").toString().compare(config.server,Qt::CaseInsensitive)!=0 || pending.value("port").toInt()!=config.port || pending.value("fingerprint").toString()!=config.pin){*error="Finish the pending identity migration before changing relay endpoint";if(replacing)QDir(stage).removeRecursively();return false;}}
    if(!stopService(error)){if(replacing)QDir(stage).removeRecursively();return false;}
    bool swapped=false,backed=false;QStringList added;
    if(replacing){QDirIterator it(stage,QDir::Files,QDirIterator::Subdirectories);while(it.hasNext()){const auto relative=QDir(stage).relativeFilePath(it.next());if(!QFile::exists(destination+"/"+relative))added<<relative;}}
    auto rollback=[&]{
        const auto original=*error;QString ignored;stopService(&ignored);
        bool restored=true;
        if(swapped){if(backed)restored=copyBundle(destination,&ignored,backup);for(const auto &relative:added)if(QFile::exists(destination+"/"+relative)&&!QFile::remove(destination+"/"+relative))restored=false;}
        const bool policyRestored=hadPolicy ? writePrivate(policyPath,oldPolicy,&ignored)
            : (!QFile::exists(policyPath)||QFile::remove(policyPath));
        const bool journalRestored=hadJournal || !QFile::exists(journalPath) || QFile::remove(journalPath);
        restored=restored && policyRestored && journalRestored;
        if(restored){
            if(QFile::exists(marker)&&!QFile::remove(marker))restored=false;
            if(restored && backed)QDir(backup).removeRecursively();
            if(restored && replacing)QDir(stage).removeRecursively();
        }else writePrivate(marker,"1",&ignored);
        if(existed && restored)startService(&ignored);else if(!existed)removeService(&ignored);
        *error=original;if(!restored)*error+="; rollback incomplete, service remains blocked pending administrator recovery";return false;
    };
    if(replacing){
        if(QFileInfo::exists(destination)){if(!trustedPath(destination,error))return rollback();if(QFile::exists(destination+"/LinkDeskService.exe")){if(!copyBundle(backup,error,destination))return rollback();backed=true;}}
        if(!writePrivate(marker,"1",error))return rollback();swapped=true;
        if(!copyBundle(destination,error,stage))return rollback();
    }
    if(!QFile::exists(dataDirectory()+"/machine-device.json") && !hadJournal && !migration.isEmpty()){
        const auto id=migration.value("id").toString(),old=migration.value("token").toString();
        if(!QRegularExpression("^[0-9]{9}$").match(id).hasMatch() || !QRegularExpression("^[0-9a-f]{64}$").match(old).hasMatch() || migration.value("server").toString().compare(config.server,Qt::CaseInsensitive)!=0 || migration.value("port").toInt()!=config.port || RelayConfig::normalizePin(migration.value("fingerprint").toString())!=config.pin){*error="Legacy identity does not match the approved server";return rollback();}
        QByteArray random;for(int i=0;i<8;++i){auto value=QRandomGenerator::system()->generate();random.append(reinterpret_cast<char *>(&value),4);}
        QJsonObject journal{{"version",1},{"id",id},{"oldToken",old},{"newToken",QString::fromLatin1(random.toHex())},{"server",config.server},{"port",int(config.port)},{"fingerprint",config.pin}};
        if(!writePrivate(journalPath,QJsonDocument(journal).toJson(QJsonDocument::Compact),error))return rollback();
    }
    if(!config.save(error)||!registerService(error))return rollback();
    if(replacing && !QFile::remove(marker)){*error="Cannot finalize service installation journal";return rollback();}
    if(!startService(error))return rollback();
    if(replacing)QDir(stage).removeRecursively();
    if(backed && !QDir(backup).removeRecursively()){*error="Service started; old protected bundle cleanup needs administrator review";return false;}
    return true;
}
static bool request(const QString &path,Config *config,QJsonObject *migration,QString *error){
    if(!noReparse(path)){*error="Unsafe setup request";return false;}QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>131072){*error="Cannot read setup request";return false;}
    const auto plain=unseal(file.readAll(),error);const auto j=QJsonDocument::fromJson(plain).object();if(j.size()!=2||!j.value("policy").isObject()||!j.value("migration").isObject()){*error="Invalid setup request";return false;}
    if(!Config::parse(j.value("policy").toObject(),config,error))return false;
    PSID owner=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;const auto native=QDir::toNativeSeparators(path).toStdWString();LPWSTR text=nullptr;bool owns=false;
    if(GetNamedSecurityInfoW(const_cast<LPWSTR>(native.c_str()),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION,&owner,nullptr,nullptr,nullptr,&descriptor)==ERROR_SUCCESS && ConvertSidToStringSidW(owner,&text))owns=QString::fromWCharArray(text)==config->operatorSid;
    if(text)LocalFree(text);if(descriptor)LocalFree(descriptor);
    if(!owns && !systemProcess(GetCurrentProcessId())){*error="Setup request owner does not match operator";return false;}
    *migration=j.value("migration").toObject();for(auto it=migration->begin();it!=migration->end();++it)if(it.key()!="id"&&it.key()!="token"&&it.key()!="server"&&it.key()!="port"&&it.key()!="fingerprint"){*error="Unknown migration field";return false;}return true;
}
int main(int argc,char **argv){
    QApplication app(argc,argv);app.setOrganizationName("LinkDesk");app.setApplicationName("LinkDeskServiceSetup");app.setStyle("Fusion");
    if(!elevated() && !systemProcess(GetCurrentProcessId())){QMessageBox::critical(nullptr,"管理员权限","请从 LinkDesk 软件设置启动服务安装，或以管理员身份运行此程序。");return 5;}
    enablePrivilege(SE_RESTORE_NAME);QString error;const auto args=app.arguments();
    if(args.size()==2 && (args[1]=="--remove"||args[1]=="--stop"||args[1]=="--start")){
        const bool ok=args[1]=="--remove"?removeService(&error):args[1]=="--stop"?stopService(&error):startService(&error);return ok?0:6;
    }
    Config config;QJsonObject migration;
    if(args.size()==3 && (args[1]=="--request"||args[1]=="--apply")){
        if(!request(args[2],&config,&migration,&error)){if(args[1]=="--request")QMessageBox::critical(nullptr,"安装请求错误",error);return 7;}
        if(args[1]=="--apply"){if(apply(config,migration,&error))return 0;qCritical().noquote()<<error;writePrivate(dataDirectory()+"/setup-error.txt",error.toUtf8(),nullptr);return 8;}
        Config existing;if(Config::load(&existing,&error))config=existing;error.clear();
    }else if(args.size()==2 && args[1]=="--configure"){
        if(!Config::load(&config,&error)){QMessageBox::critical(nullptr,"服务未配置","请先从普通用户的 LinkDesk 设置创建安装请求。");return 9;}
    }else return 2;
    QDialog dialog;dialog.setWindowTitle("LinkDesk 无人值守服务 · 管理员配置");auto layout=new QFormLayout(&dialog);
    auto server=new QLineEdit(config.server);auto port=new QLineEdit(QString::number(config.port));auto pin=new QLineEdit(config.pin);auto key=new QLineEdit(config.enrollment);key->setEchoMode(QLineEdit::Password);auto password=new QLineEdit(config.password);password->setEchoMode(QLineEdit::Password);
    layout->addRow("中转服务器",server);layout->addRow("端口",port);layout->addRow("服务器 SHA-256 指纹",pin);layout->addRow("服务器接入密钥",key);layout->addRow("机器远控连接密码",password);
    auto unattended=new QCheckBox("授权无人值守：登录屏幕、锁屏与 UAC 的画面及鼠标键盘控制");unattended->setChecked(config.unattended);layout->addRow(unattended);
    auto files=new QCheckBox("允许文件传输（仅以登录用户身份访问其文件；登录前禁用）");files->setChecked(config.files);layout->addRow(files);
    auto power=new QCheckBox("单独授权重启与关机操作");power->setChecked(config.power);layout->addRow(power);
    auto sas=new QCheckBox("授权系统 SendSAS（还需 Windows 策略允许；不会自动修改策略）");sas->setChecked(config.sas);layout->addRow(sas);
    auto mode=new QComboBox;mode->addItem("仅当前控制台","console");mode->addItem("控制台优先，跟随活动交互/RDP 会话","active");mode->setCurrentIndex(config.sessionMode=="active"?1:0);layout->addRow("会话选择",mode);
    auto keepId=new QCheckBox("保留原设备 ID，并轮换用户级注册凭据（需要新版中转服务）");keepId->setChecked(!migration.isEmpty());keepId->setEnabled(!migration.isEmpty()&&!QFile::exists(dataDirectory()+"/machine-device.json"));layout->addRow(keepId);
    auto note=new QLabel("服务由 SCM 在开机时启动。不会保存 Windows 登录密码。\n文件功能保留在登录用户进程中。机器身份与个人查看端数据独立。\n保留原 ID 时轮换机器凭据与 TLS 私钥；不保留时需要更新连接列表。\n停止或卸载服务会停止全部会话工作进程，机器私有配置保留以便重新安装。");note->setWordWrap(true);layout->addRow(note);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel);layout->addRow(buttons);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        auto j=config.json();j["server"]=server->text().trimmed();j["port"]=port->text().toInt();j["fingerprint"]=pin->text();j["enrollmentKey"]=key->text();j["password"]=password->text();j["unattended"]=unattended->isChecked();j["files"]=files->isChecked();j["power"]=power->isChecked();j["sas"]=sas->isChecked();j["sessionMode"]=mode->currentData().toString();
        Config chosen;QString reason;if(!Config::parse(j,&chosen,&reason)){QMessageBox::warning(&dialog,"设置无效",reason);return;}
        if(!apply(chosen,keepId->isChecked()?migration:QJsonObject(),&reason)){QMessageBox::critical(&dialog,"服务配置失败",reason);return;}dialog.accept();
    });
    dialog.resize(780,600);return dialog.exec()==QDialog::Accepted?0:1;
}
