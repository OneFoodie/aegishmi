#include "infra/modbus_tcp_client.h"

#include "protocol/modbus.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QTcpSocket>
#include <QTimer>

namespace aegis::infra {

namespace {
constexpr int kPollIntervalMs = 20;    // 50Hz 轮询
constexpr int kRequestTimeoutMs = 500; // 单次轮询超时后允许重新发
} // namespace

ModbusTcpClient::ModbusTcpClient(QObject* parent)
    : QObject(parent)
    , socket_(new QTcpSocket(this))
    , timer_(new QTimer(this))
{
    timer_->setInterval(kPollIntervalMs);
    connect(timer_, &QTimer::timeout, this, &ModbusTcpClient::poll);

    connect(socket_, &QTcpSocket::connected, this, &ModbusTcpClient::onConnected);
    connect(socket_, &QTcpSocket::readyRead, this, &ModbusTcpClient::onReadyRead);

    connect(socket_, &QTcpSocket::disconnected, this, [this] {
        timer_->stop();
        pollPending_ = false;
        pendingWriteTx_ = 0;
        pendingReadTx_ = 0;
        if (connected_) {
            connected_ = false;
            emit closed();
        }
    });

    connect(socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit failed(socket_->errorString());
    });
}

ModbusTcpClient::~ModbusTcpClient()
{
    socket_->abort();
}

void ModbusTcpClient::open(const QString& host, quint16 port, int unitId)
{
    unitId_ = (unitId >= 1 && unitId <= 247) ? unitId : 1;

    if (socket_->state() != QAbstractSocket::UnconnectedState)
        socket_->abort();
    buffer_.clear();
    pollPending_ = false;
    pendingWriteTx_ = 0;
    pendingReadTx_ = 0;

    socket_->connectToHost(host, port);
}

void ModbusTcpClient::close()
{
    timer_->stop();
    pollPending_ = false;
    pendingWriteTx_ = 0;
    pendingReadTx_ = 0;
    socket_->abort();
}

void ModbusTcpClient::onConnected()
{
    connected_ = true;
    emit opened();
    timer_->start();
}

void ModbusTcpClient::writeRegister(quint16 transaction, quint16 address, quint16 value)
{
    if (!connected_) {
        emit writeCompleted(transaction, false, 0xFF); // 0xFF 表示链路不可用
        return;
    }

    protocol::modbus::Request request;
    request.unitId = static_cast<std::uint8_t>(unitId_);
    request.function = protocol::modbus::FunctionCode::WriteSingleRegister;
    request.startAddress = address;
    request.writeValue = value;

    const auto bytes = protocol::modbus::encodeTcpRequest(transaction, request);
    socket_->write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<qint64>(bytes.size()));
    pendingWriteTx_ = transaction;
}

void ModbusTcpClient::readHoldingRegisters(quint16 transaction, quint16 address, quint16 count)
{
    if (!connected_) {
        emit readCompleted(transaction, false, {});
        return;
    }

    protocol::modbus::Request request;
    request.unitId = static_cast<std::uint8_t>(unitId_);
    request.function = protocol::modbus::FunctionCode::ReadHoldingRegisters;
    request.startAddress = address;
    request.quantity = count;

    const auto bytes = protocol::modbus::encodeTcpRequest(transaction, request);
    socket_->write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<qint64>(bytes.size()));
    pendingReadTx_ = transaction;
    pendingSinceMs_ = QDateTime::currentMSecsSinceEpoch();
}

void ModbusTcpClient::poll()
{
    if (!connected_)
        return;

    // 有读请求在途时暂停轮询：避免响应交错，也减少设备压力。
    // 超时未回应则清理标记，避免轮询永久停摆。
    if (pendingReadTx_ != 0) {
        if (QDateTime::currentMSecsSinceEpoch() - pendingSinceMs_ < kRequestTimeoutMs)
            return;
        const std::uint16_t transaction = pendingReadTx_;
        pendingReadTx_ = 0;
        emit readCompleted(transaction, false, {});
    }

    // 上一次轮询未回应：未超时跳过本次，超时允许重发。
    if (pollPending_) {
        if (QDateTime::currentMSecsSinceEpoch() - pendingSinceMs_ < kRequestTimeoutMs)
            return;
        pollPending_ = false;
    }

    ++transactionId_;
    // 避免与写事务号撞车：轮询事务号只使用 0x0001~0x7FFF。
    if (transactionId_ == 0 || transactionId_ > 0x7FFF)
        transactionId_ = 1;

    pendingPollTx_ = transactionId_;

    protocol::modbus::Request request;
    request.unitId = static_cast<std::uint8_t>(unitId_);
    request.function = protocol::modbus::FunctionCode::ReadInputRegisters;
    request.startAddress = startAddress_;
    request.quantity = registerCount_;

    const auto bytes = protocol::modbus::encodeTcpRequest(pendingPollTx_, request);
    socket_->write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<qint64>(bytes.size()));

    pollPending_ = true;
    pendingSinceMs_ = QDateTime::currentMSecsSinceEpoch();
}

void ModbusTcpClient::onReadyRead()
{
    buffer_.append(socket_->readAll());

    while (true) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(buffer_.constData());
        const std::size_t size = static_cast<std::size_t>(buffer_.size());
        const std::size_t frameLength = protocol::modbus::tcpFrameLength(data, size);
        if (frameLength == 0)
            break; // 半包

        const std::uint16_t transaction =
            static_cast<std::uint16_t>((data[0] << 8) | data[1]);

        protocol::modbus::Response response;
        const bool ok = protocol::modbus::decodeTcpResponse(
            data, frameLength, transaction, response);

        buffer_.remove(0, static_cast<int>(frameLength));

        if (!ok)
            continue;

        // 写操作的响应：回显地址/值，或异常码。
        if (pendingWriteTx_ != 0 && transaction == pendingWriteTx_) {
            pendingWriteTx_ = 0;
            emit writeCompleted(transaction, !response.isException,
                                response.isException ? response.exceptionCode : 0);
            continue;
        }

        // 读保持寄存器（FC03）的响应。
        if (pendingReadTx_ != 0 && transaction == pendingReadTx_) {
            pendingReadTx_ = 0;

            QVector<quint16> registers;
            if (!response.isException) {
                registers.reserve(static_cast<int>(response.registers.size()));
                for (const auto value : response.registers)
                    registers.append(value);
            }
            emit readCompleted(transaction, !response.isException, registers);
            continue;
        }

        // 轮询响应。
        if (transaction == pendingPollTx_) {
            pollPending_ = false;

            if (response.isException) {
                emit failed(QStringLiteral("Modbus 异常码 %1")
                                .arg(static_cast<int>(response.exceptionCode)));
                continue;
            }

            QVector<quint16> registers;
            registers.reserve(static_cast<int>(response.registers.size()));
            for (const auto value : response.registers)
                registers.append(value);
            emit registersUpdated(registers);
        }
    }
}

} // namespace aegis::infra
