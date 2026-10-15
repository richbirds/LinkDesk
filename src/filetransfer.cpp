#include "filetransfer.h"
#include "protocol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QtConcurrent>
#include <QtEndian>

static QByteArray digestFile(const QString &path,const std::shared_ptr<std::atomic_bool> &cancel) {
    QFile f(path); if(!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while(!f.atEnd()) {
        if(cancel->load(std::memory_order_relaxed)) return {};
        const auto block=f.read(256*1024);if(block.isEmpty() && f.error()!=QFileDevice::NoError)return {};
        hash.addData(block);
    }
    return cancel->load(std::memory_order_relaxed) ? QByteArray() : hash.result().toHex();
}
QString FileTransfer::receiveDirectory() {
    return QSettings().value("files/receiveDirectory",QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)).toString();
}
FileTransfer::FileTransfer(QObject *parent):QObject(parent) {
    m_pump.setInterval(10); connect(&m_pump,&QTimer::timeout,this,&FileTransfer::pump); m_pump.start();
    m_retry.setInterval(15000); connect(&m_retry,&QTimer::timeout,this,[this]{if(!m_ready) offer();});m_retry.start();
}
FileTransfer::~FileTransfer() {shutdown();}
void FileTransfer::shutdown() {
    if(m_stopped) return;m_stopped=true;
    cancel();if(m_receiveCancel)m_receiveCancel->store(true);
    m_pump.stop();m_retry.stop();if(m_part.isOpen())m_part.flush();m_part.close();
    if(m_wire) disconnect(m_wire,nullptr,this,nullptr);m_wire.clear();
}
void FileTransfer::attach(Wire *wire) {
    if(m_stopped) return;
    if(m_wire) disconnect(m_wire,nullptr,this,nullptr);
    m_wire=wire; m_ready=false; m_source.close(); m_part.close();m_inId.clear();
    if(!wire) return;
    connect(wire,&Wire::jsonReceived,this,&FileTransfer::message);
    connect(wire,&Wire::fileReceived,this,&FileTransfer::chunk);
    offer();next();
}
void FileTransfer::cancel() {if(m_sendCancel)m_sendCancel->store(true);++m_generation;m_queue.clear();m_source.close();m_id.clear();m_ready=false;m_preparing=false;}
void FileTransfer::enqueue(const QStringList &paths) {
    if(m_stopped)return;
    for(const auto &path:paths) { const QFileInfo f(path); if(f.isFile() && !f.isSymLink()) m_queue.enqueue(f.absoluteFilePath()); }
    next();
}
void FileTransfer::next() {
    if(m_stopped || m_preparing || !m_id.isEmpty() || m_queue.isEmpty()) return;
    const QString path=m_queue.dequeue();const auto generation=m_generation; m_preparing=true;
    emit progress(QFileInfo(path).fileName(),0,QFileInfo(path).size(),"正在校验文件…");
    m_sendCancel=std::make_shared<std::atomic_bool>(false);
    auto watcher=new QFutureWatcher<QByteArray>(this);
    connect(watcher,&QFutureWatcher<QByteArray>::finished,this,[this,watcher,path,generation]{
        if(generation!=m_generation) {watcher->deleteLater();return;}
        m_preparing=false; const auto hash=watcher->result();watcher->deleteLater();
        if(hash.isEmpty()) { emit progress(QFileInfo(path).fileName(),0,0,"无法读取文件");next();return; }
        m_hash=QString::fromLatin1(hash);m_name=QFileInfo(path).fileName();m_size=QFileInfo(path).size();
        m_id=QString::fromLatin1(QCryptographicHash::hash(hash+m_name.toUtf8(),QCryptographicHash::Sha256).toHex());
        m_source.setFileName(path); m_ack=m_sent=0;offer();
    });watcher->setFuture(QtConcurrent::run(digestFile,path,m_sendCancel));
}
void FileTransfer::offer() {
    if(!m_wire || m_id.isEmpty()) return;
    m_wire->sendJson({{"type","file_offer"},{"id",m_id},{"name",m_name},{"size",QString::number(m_size)},{"sha256",m_hash}});
    emit progress(m_name,m_ack,m_size,"等待接收 / 自动续传");
}
void FileTransfer::fail(const QString &reason) {
    emit progress(m_name,m_ack,m_size,reason);m_source.close();m_id.clear();m_ready=false;next();
}
void FileTransfer::reply(const QString &type,qint64 offset,const QString &error) {
    if(m_wire) m_wire->sendJson({{"type",type},{"id",m_inId},{"offset",QString::number(offset)},{"message",error}});
}
void FileTransfer::message(const QJsonObject &j) {
    if(m_stopped)return;
    const auto type=j["type"].toString(), id=j["id"].toString();
    if(type=="file_offer") {
        if(m_finishing) return;
        static const QRegularExpression hex("^[a-f0-9]{64}$");
        const auto name=j["name"].toString(),hash=j["sha256"].toString();bool valid=false;
        const qint64 size=j["size"].toString().toLongLong(&valid);
        if(!hex.match(id).hasMatch() || !hex.match(hash).hasMatch() || !valid || size<0 || size>qint64(1024)*1024*1024*1024 || name.isEmpty() || name.size()>180 || name.contains('/') || name.contains('\\') || name.contains(':') || name=="." || name==".." || name.contains(QChar(0)) || name.endsWith('.') || name.endsWith(' ') || QRegularExpression("[\\x00-\\x1f]").match(name).hasMatch() || QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)",QRegularExpression::CaseInsensitiveOption).match(name).hasMatch()) {
            if(hex.match(id).hasMatch() && m_wire) m_wire->sendJson({{"type","file_error"},{"id",id},{"message","文件名或大小无效"}});return;
        }
        if(id!=QString::fromLatin1(QCryptographicHash::hash(hash.toLatin1()+name.toUtf8(),QCryptographicHash::Sha256).toHex())) return;
        m_part.close(); m_inId=id;m_inHash=hash;m_inSize=size;m_inName=name;
        m_directory=receiveDirectory();QDir root(m_directory);
        if(!root.exists() && !QDir().mkpath(m_directory)) {reply("file_error",0,"接收目录不可写");return;}
        const QString staging=root.filePath(".linkdesk-partials");
        if(QFileInfo(staging).isSymLink() || !QDir().mkpath(staging)) {reply("file_error",0,"临时目录不可写");return;}
        const QString partial=QDir(staging).filePath(id+".part");
        if(QFileInfo(partial).isSymLink()) {reply("file_error",0,"无效的临时文件");return;}
        m_part.setFileName(partial);
        if(!m_part.open(QIODevice::ReadWrite)) {reply("file_error",0,"无法写入接收文件");return;}
        m_part.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        if(m_part.size()>size) m_part.resize(0);
        m_part.seek(m_part.size());reply("file_ready",m_part.pos());
    } else if(id==m_id && !id.isEmpty()) {
        if(type=="file_ready") {
            bool ok=false;const auto offset=j["offset"].toString().toLongLong(&ok);
            if(!ok || offset<0 || offset>m_size) {fail("接收进度无效");return;}
            if(!m_source.isOpen() && !m_source.open(QIODevice::ReadOnly)) {fail("源文件无法打开");return;}
            if(m_source.size()!=m_size || !m_source.seek(offset)) {fail("源文件已改变，请重新发送");return;}
            m_ack=m_sent=offset;m_ready=true;
        } else if(type=="file_ack") {
            bool ok=false;const auto offset=j["offset"].toString().toLongLong(&ok);
            if(ok && offset>=m_ack && offset<=m_sent) {m_ack=offset;emit progress(m_name,m_ack,m_size,"传送中");}
        } else if(type=="file_done") {emit progress(m_name,m_size,m_size,"传送完成");m_source.close();m_id.clear();m_ready=false;next();}
        else if(type=="file_error") fail("传送失败："+j["message"].toString().left(160));
    }
    if(type=="file_finish" && id==m_inId && m_part.isOpen() && !m_finishing) {
        if(m_part.pos()!=m_inSize || !m_part.flush()) {reply("file_error",0,"文件长度或磁盘写入异常");m_part.close();return;}
        const QString partial=m_part.fileName();m_part.close();m_finishing=true;
        m_receiveCancel=std::make_shared<std::atomic_bool>(false);
        auto watcher=new QFutureWatcher<QByteArray>(this);
        connect(watcher,&QFutureWatcher<QByteArray>::finished,this,[this,watcher,partial]{
            if(m_stopped){watcher->deleteLater();return;}
            const auto hash=watcher->result();watcher->deleteLater();m_finishing=false;
            if(hash!=m_inHash.toLatin1()) {QFile::remove(partial);reply("file_error",0,"完整性校验失败，请重新发送");return;}
            const QFileInfo original(m_inName);QString destination;
            for(int n=0;n<10000;++n) {
                const auto name=n ? original.completeBaseName()+" ("+QString::number(n)+")"+(original.suffix().isEmpty()?QString():"."+original.suffix()) : m_inName;
                destination=QDir(m_directory).filePath(name);
                if(QFile::rename(partial,destination)) {reply("file_done",m_inSize);emit received(destination);return;}
                if(!QFileInfo::exists(destination)) break;
            }reply("file_error",0,"无法保存文件，接收目录可能已满");
        });watcher->setFuture(QtConcurrent::run(digestFile,partial,m_receiveCancel));
    }
}
void FileTransfer::pump() {
    if(!m_wire || !m_ready || !m_source.isOpen()) return;
    // Bound data in flight; leave space for input, capture and heartbeat packets.
    for(int n=0;n<4 && m_sent<m_size && m_sent-m_ack<256*1024;++n) {
        const auto data=m_source.read(qMin<qint64>(32768,m_size-m_sent));
        if(data.isEmpty()) {fail("源文件读取失败");return;}
        QByteArray packet=m_id.toLatin1();char offset[8];qToBigEndian<quint64>(m_sent,offset);packet.append(offset,8);packet+=data;
        if(!m_wire->sendFile(packet)) {m_source.seek(m_sent);return;}m_sent+=data.size();
    }
    if(m_sent==m_size && m_ack==m_size) {
        m_wire->sendJson({{"type","file_finish"},{"id",m_id}});m_ready=false;
        emit progress(m_name,m_ack,m_size,"对方正在校验…");
    }
}
void FileTransfer::chunk(const QByteArray &data) {
    if(m_stopped || !m_part.isOpen() || data.size()<=72 || data.size()>32840 || data.left(64)!=m_inId.toLatin1()) return;
    const quint64 offset=qFromBigEndian<quint64>(data.constData()+64);const auto payload=data.mid(72);
    if(offset!=quint64(m_part.pos()) || offset+payload.size()>quint64(m_inSize)) {reply("file_error",0,"文件偏移异常");m_part.close();return;}
    if(m_part.write(payload)!=payload.size() || !m_part.flush()) {reply("file_error",0,"磁盘写入失败");m_part.close();return;}
    reply("file_ack",m_part.pos());emit progress(m_inName,m_part.pos(),m_inSize,"接收中");
}
