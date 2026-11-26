#include "infra/serial_transport.h"

#include <QSerialPort> // QSerialPort：Qt 提供的跨平台串口类

namespace aegis::infra {

SerialTransport::SerialTransport(QObject* parent)
    : QObject(parent)
    , port_(new QSerialPort(this)) // 父对象为 this，自动随本对象销毁
{
    // 串口有数据可读：一次性取走缓冲区内容并发给上层解码。
    connect(port_, &QSerialPort::readyRead, this, [this] {
        const QByteArray data = port_->readAll();
        if (!data.isEmpty())
            emit bytesReceived(data);
    });

    // 串口错误回调。NoError 属于"正常状态通知"，不是错误，直接忽略。
    connect(port_, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError)
            return;
        emit failed(port_->errorString());
    });
}

SerialTransport::~SerialTransport()
{
    // 析构前主动关闭，避免串口句柄泄漏（Windows 下会导致下次无法打开同一端口）。
    if (port_->isOpen())
        port_->close();
}

void SerialTransport::open(const QString& portName, int baudRate)
{
    // 若已打开，先关闭再重新配置。
    if (port_->isOpen())
        port_->close();

    port_->setPortName(portName);   // 端口名，例如 "COM3"
    port_->setBaudRate(baudRate);   // 波特率必须与设备一致，否则收到乱码
    port_->setDataBits(QSerialPort::Data8);   // 8 位数据位（最常用）
    port_->setParity(QSerialPort::NoParity);  // 无校验；工业设备也常见偶校验 Even
    port_->setStopBits(QSerialPort::OneStop); // 1 位停止位
    port_->setFlowControl(QSerialPort::NoFlowControl); // 无流控（不用 RTS/CTS）

    // open 成功返回 true；失败时用 errorString() 给出原因（端口被占用/不存在等）。
    if (port_->open(QIODevice::ReadWrite)) {
        emit opened();
    } else {
        emit failed(port_->errorString());
    }
}

void SerialTransport::close()
{
    if (!port_->isOpen())
        return;
    port_->close();
    emit closed();
}

} // namespace aegis::infra
