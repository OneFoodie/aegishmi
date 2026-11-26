#include "infra/tcp_transport.h"

#include <QAbstractSocket> // QAbstractSocket::UnconnectedState 等状态枚举
#include <QTcpSocket>      // TCP 客户端套接字（需要完整定义）

namespace aegis::infra {

// 构造函数：创建 socket，并把它的内部信号转发为本类的信号。
TcpTransport::TcpTransport(QObject* parent)
    : QObject(parent)                    // 先构造基类
    , socket_(new QTcpSocket(this))      // 传 this 作父对象，socket 随本对象一起销毁
{
    // connect(发送者, 信号, 接收者, 槽/函数)
    // 下面四行把 QTcpSocket 的底层事件"翻译"成业务层能理解的信号。

    // 1) 连接建立 -> 发 opened
    connect(socket_, &QTcpSocket::connected, this, &TcpTransport::opened);

    // 2) 连接断开 -> 发 closed
    connect(socket_, &QTcpSocket::disconnected, this, &TcpTransport::closed);

    // 3) 有数据可读 -> readAll() 取出全部字节 -> 发 bytesReceived
    //    lambda 捕获 this，在 this 所在线程执行；readyRead 会在对象所属线程的事件循环中触发。
    connect(socket_, &QTcpSocket::readyRead, this, [this] {
        const QByteArray data = socket_->readAll();
        if (!data.isEmpty())
            emit bytesReceived(data); // emit 只是语法糖，本质是调用 moc 生成的信号函数
    });

    // 4) 发生错误 -> 发 failed，把英文错误转成 QString 交给上层显示。
    //    参数 SocketError 用不到，省略参数名避免"未使用变量"警告。
    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit failed(socket_->errorString());
    });
}

// 析构时确保 socket 不会继续工作。abort() 立即关闭，不发 FIN 四次挥手。
TcpTransport::~TcpTransport()
{
    socket_->abort();
}

void TcpTransport::open(const QString& host, quint16 port)
{
    // 如果已经处于连接中/已连接状态，先强制复位，避免重复连接产生多个 socket 状态。
    if (socket_->state() != QAbstractSocket::UnconnectedState)
        socket_->abort();

    // connectToHost 是异步的：调用后立即返回，
    // 真正的结果通过 connected / errorOccurred 信号通知，不会阻塞线程。
    socket_->connectToHost(host, port);
}

void TcpTransport::close()
{
    // abort() 会关闭连接；若当前未连接，调用也是安全的（无副作用）。
    // 若希望"优雅断开"，可改用 disconnectFromHost()，但本项目追求快速重连，故用 abort。
    socket_->abort();
}

void TcpTransport::send(const QByteArray& data)
{
    // 只在已连接时写入；未连接时直接丢弃，由上层根据超时判定失败。
    if (socket_->state() == QAbstractSocket::ConnectedState)
        socket_->write(data);
}

} // namespace aegis::infra
