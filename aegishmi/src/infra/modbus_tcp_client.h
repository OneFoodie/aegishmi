#pragma once // 头文件保护

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

class QTcpSocket;
class QTimer;

namespace aegis::infra {

// =============================================================================
// ModbusTcpClient —— Modbus TCP 主站
//
// 功能：
//   1. 20ms 周期轮询 FC04 读输入寄存器（地址 0 起 6 个）
//   2. 按请求写单个寄存器（FC06），用于测试流程下发命令
//   3. 按请求读保持寄存器（FC03），用于读取版本号等信息
//
// 请求/响应靠事务号配对：轮询、写操作、读操作各用一个事务号，互不干扰。
// 本对象由 DeviceService 移动到通信线程中运行。
// =============================================================================
class ModbusTcpClient : public QObject {
    Q_OBJECT

public:
    explicit ModbusTcpClient(QObject* parent = nullptr);
    ~ModbusTcpClient() override;

public slots:
    void open(const QString& host, quint16 port, int unitId);
    void close();
    // 写单个寄存器；transaction 由调用方提供，响应时原样带回。
    void writeRegister(quint16 transaction, quint16 address, quint16 value);
    // 读保持寄存器（FC03）：transaction 非 0，响应时原样带回。
    void readHoldingRegisters(quint16 transaction, quint16 address, quint16 count);

signals:
    void opened();
    void closed();
    void failed(const QString& reason);
    void registersUpdated(const QVector<quint16>& registers);
    void writeCompleted(quint16 transaction, bool ok, quint8 status);
    // 读保持寄存器结果：异常响应或链路不可用时 ok=false。
    void readCompleted(quint16 transaction, bool ok, const QVector<quint16>& registers);

private slots:
    void onConnected();
    void onReadyRead();
    void poll();

private:
    QTcpSocket* socket_ = nullptr;
    QTimer* timer_ = nullptr;
    QByteArray buffer_;

    std::uint16_t transactionId_ = 0;   // 轮询事务号自增
    std::uint16_t pendingPollTx_ = 0;   // 正在等待的轮询事务号
    std::uint16_t pendingWriteTx_ = 0;  // 正在等待的写事务号（0 表示没有）
    std::uint16_t pendingReadTx_ = 0;   // 正在等待的读事务号（0 表示没有）
    bool pollPending_ = false;
    qint64 pendingSinceMs_ = 0;

    int unitId_ = 1;
    std::uint16_t startAddress_ = 0;
    std::uint16_t registerCount_ = 6;
    bool connected_ = false;
};

} // namespace aegis::infra
