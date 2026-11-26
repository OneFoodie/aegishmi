#pragma once // 头文件保护

#include <QByteArray>
#include <QObject>
#include <QString>

class QSerialPort; // 前置声明

namespace aegis::infra {

// =============================================================================
// SerialTransport —— 串口通信封装
//
// 与 TcpTransport 结构完全一致（同样的信号集合），
// 这样 DeviceService 未来可以按配置在"串口 / TCP"之间切换，
// 上层代码不需要关心底层是哪种物理链路（依赖倒置）。
//
// 当前主界面只用了 TCP，串口实现作为真实设备接入的预留能力。
// =============================================================================
class SerialTransport : public QObject {
    Q_OBJECT

public:
    explicit SerialTransport(QObject* parent = nullptr);
    ~SerialTransport() override;

public slots:
    // portName 形如 "COM3"；baudRate 常用 9600 / 115200 / 921600。
    void open(const QString& portName, int baudRate);
    void close();

signals:
    void opened();                              // 串口打开成功
    void closed();                              // 串口关闭
    void failed(const QString& reason);         // 打开失败或运行期错误
    void bytesReceived(const QByteArray& data); // 收到原始字节

private:
    QSerialPort* port_ = nullptr;
};

} // namespace aegis::infra
