#pragma once // 头文件保护

#include <QByteArray>
#include <QObject>
#include <QString>

class QTcpSocket;

namespace aegis::infra {

// =============================================================================
// IScpiTransport —— SCPI 传输层抽象
//
// ScpiClient 只负责 SCPI 报文（排队、超时、按行解析），具体怎么收发由传输层
// 决定：
//   - TcpScpiTransport：SCPI-RAW over TCP（默认，端口 5025）；
//   - 后续可接 VISA（GPIB/USBTMC）适配层或测试用 mock。
// 厂商驱动缺失时只需让对应传输层打开失败，不影响主程序编译与其它链路。
// =============================================================================
class IScpiTransport : public QObject {
    Q_OBJECT

public:
    explicit IScpiTransport(QObject* parent = nullptr)
        : QObject(parent)
    {
    }
    ~IScpiTransport() override = default;

    // address 的含义由实现决定：TCP 为 host，VISA 为资源串（如 GPIB0::22::INSTR）。
    virtual void open(const QString& address, quint16 port) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual void write(const QByteArray& data) = 0;
    virtual QString errorString() const = 0;
    // tcp / visa / usbtmc，用于日志与界面提示。
    virtual QString transportName() const = 0;

signals:
    void opened();
    void closed();
    void failed(const QString& reason);
    void dataReady(const QByteArray& data);
};

// =============================================================================
// TcpScpiTransport —— SCPI-RAW over TCP（现有 LAN 仪器路径）
// =============================================================================
class TcpScpiTransport final : public IScpiTransport {
    Q_OBJECT

public:
    explicit TcpScpiTransport(QObject* parent = nullptr);

    void open(const QString& address, quint16 port) override;
    void close() override;
    bool isOpen() const override;
    void write(const QByteArray& data) override;
    QString errorString() const override;
    QString transportName() const override;

private:
    QTcpSocket* socket_ = nullptr;
};

} // namespace aegis::infra
