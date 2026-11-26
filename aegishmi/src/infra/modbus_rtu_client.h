#pragma once // 头文件保护

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

class QSerialPort;
class QTimer;

namespace aegis::infra {

// =============================================================================
// ModbusRtuClient —— Modbus RTU 主站（串口）
//
// 与 ModbusTcpClient 行为一致：周期轮询 FC04 读寄存器，支持 FC06 写单个寄存器、
// FC03 读保持寄存器（版本号等）。
// 差异：
//   - 物理层是串口（QSerialPort），需要端口名与波特率
//   - RTU 帧没有事务号，靠"发出请求 -> 等一帧响应"的顺序配对
//
// 轮询周期 50ms（20Hz）：串口链路比特率有限，过快的轮询会互相淹没。
// 本对象由 DeviceService 移动到通信线程中运行。
// =============================================================================
class ModbusRtuClient : public QObject {
    Q_OBJECT

public:
    explicit ModbusRtuClient(QObject* parent = nullptr);
    ~ModbusRtuClient() override;

public slots:
    void open(const QString& portName, int baudRate, int unitId);
    void close();
    void writeRegister(quint16 transaction, quint16 address, quint16 value);
    // 读保持寄存器（FC03）；transaction 仅用于上层配对，不进入 RTU 帧。
    void readHoldingRegisters(quint16 transaction, quint16 address, quint16 count);

signals:
    void opened();
    void closed();
    void failed(const QString& reason);
    void registersUpdated(const QVector<quint16>& registers);
    void writeCompleted(quint16 transaction, bool ok, quint8 status);
    void readCompleted(quint16 transaction, bool ok, const QVector<quint16>& registers);

private slots:
    void poll();
    void onReadyRead();

private:
    QSerialPort* port_ = nullptr;
    QTimer* timer_ = nullptr;
    QByteArray buffer_;

    int unitId_ = 1;
    bool connected_ = false;
    bool pollPending_ = false;
    qint64 pendingSinceMs_ = 0;
    std::uint16_t pendingWriteTx_ = 0;
    bool readPending_ = false;               // 读保持寄存器请求在途
    std::uint16_t pendingReadTx_ = 0;        // 读请求的事务号（仅上层配对用）
};

} // namespace aegis::infra
