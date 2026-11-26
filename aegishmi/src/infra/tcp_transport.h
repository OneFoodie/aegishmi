#pragma once // 头文件保护

#include <QByteArray> // Qt 的字节数组，用于承载一次收到的原始字节
#include <QObject>    // Qt 对象基类：提供信号槽、线程归属等机制
#include <QString>    // 设备地址等字符串

class QTcpSocket; // 前置声明：头文件里不暴露 QTcpSocket 的完整定义，减少编译依赖

namespace aegis::infra {

// =============================================================================
// TcpTransport —— TCP 客户端通信封装
//
// 设计要点：
//  1. 继承 QObject 并写 Q_OBJECT 宏：这样才能定义"信号"（signal），并使用信号槽机制。
//  2. 本对象会被 DeviceService 移动到独立线程（moveToThread），
//     这样网络读写不会阻塞 UI 线程。
//  3. 对外只暴露"打开/关闭"两个槽函数和几个信号；
//     UI 层不需要知道 QTcpSocket 的存在，方便以后替换成串口/Modbus。
// =============================================================================
class TcpTransport : public QObject {
    Q_OBJECT // 必须写：moc 工具据此生成元对象代码（信号槽、属性系统的基础）

public:
    // parent 用于 Qt 对象树：父对象析构时会自动 delete 子对象。
    explicit TcpTransport(QObject* parent = nullptr);
    ~TcpTransport() override; // override 表示重写基类虚函数，写错签名会编译报错

public slots: // slots 是槽函数：可以被信号连接，也可以被 QMetaObject::invokeMethod 跨线程调用
    // 连接服务器。host 支持域名或 IP，port 为端口号。
    void open(const QString& host, quint16 port);
    // 立即断开连接。
    void close();
    // 发送一段字节。必须在传输线程中调用（通过 invokeMethod 投递）。
    void send(const QByteArray& data);

signals: // 信号：只声明不实现，由 moc 生成实现；发射时用 emit 关键字
    void opened();                             // 连接建立成功
    void closed();                             // 连接断开
    void failed(const QString& reason);        // 发生错误（含连接失败、连接被重置等）
    void bytesReceived(const QByteArray& data); // 收到一段原始字节（未解析）

private:
    QTcpSocket* socket_ = nullptr; // 底层 socket；生命周期由对象树管理（this 为父对象）
};

} // namespace aegis::infra
