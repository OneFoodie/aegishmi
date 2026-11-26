#include "infra/modbus_rtu_client.h"

#include "protocol/modbus.h"

#include <QDateTime>
#include <QSerialPort>
#include <QTimer>

namespace aegis::infra {

namespace {
constexpr int kPollIntervalMs = 50;    // 20Hz，串口链路留足响应时间
constexpr int kRequestTimeoutMs = 800; // 串口单次请求超时
} // namespace

ModbusRtuClient::ModbusRtuClient(QObject* parent)
    : QObject(parent)
    , port_(new QSerialPort(this))
    , timer_(new QTimer(this))
{
    timer_->setInterval(kPollIntervalMs);
    connect(timer_, &QTimer::timeout, this, &ModbusRtuClient::poll);
    connect(port_, &QSerialPort::readyRead, this, &ModbusRtuClient::onReadyRead);

    connect(port_, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError)
            return;
        emit failed(port_->errorString());
    });
}

ModbusRtuClient::~ModbusRtuClient()
{
    if (port_->isOpen())
        port_->close();
}

void ModbusRtuClient::open(const QString& portName, int baudRate, int unitId)
{
    unitId_ = (unitId >= 1 && unitId <= 247) ? unitId : 1;

    if (port_->isOpen()) {
        timer_->stop();
        port_->close();
    }

    port_->setPortName(portName);
    port_->setBaudRate(baudRate);
    port_->setDataBits(QSerialPort::Data8);
    port_->setParity(QSerialPort::NoParity);
    port_->setStopBits(QSerialPort::OneStop);
    port_->setFlowControl(QSerialPort::NoFlowControl);

    buffer_.clear();
    pollPending_ = false;
    pendingWriteTx_ = 0;
    readPending_ = false;
    pendingReadTx_ = 0;

    if (!port_->open(QIODevice::ReadWrite)) {
        emit failed(port_->errorString());
        return;
    }

    connected_ = true;
    emit opened();
    timer_->start();
}

void ModbusRtuClient::close()
{
    if (!connected_ && !port_->isOpen())
        return;

    timer_->stop();
    connected_ = false;
    pollPending_ = false;
    pendingWriteTx_ = 0;
    readPending_ = false;
    pendingReadTx_ = 0;
    port_->close();
    emit closed();
}

void ModbusRtuClient::writeRegister(quint16 transaction, quint16 address, quint16 value)
{
    if (!connected_ || !port_->isOpen()) {
        emit writeCompleted(transaction, false, 0xFF);
        return;
    }

    protocol::modbus::Request request;
    request.unitId = static_cast<std::uint8_t>(unitId_);
    request.function = protocol::modbus::FunctionCode::WriteSingleRegister;
    request.startAddress = address;
    request.writeValue = value;

    const auto bytes = protocol::modbus::encodeRtuRequest(request);
    port_->write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<qint64>(bytes.size()));
    pendingWriteTx_ = transaction;
}

void ModbusRtuClient::readHoldingRegisters(quint16 transaction, quint16 address,
                                           quint16 count)
{
    if (!connected_ || !port_->isOpen()) {
        emit readCompleted(transaction, false, {});
        return;
    }

    protocol::modbus::Request request;
    request.unitId = static_cast<std::uint8_t>(unitId_);
    request.function = protocol::modbus::FunctionCode::ReadHoldingRegisters;
    request.startAddress = address;
    request.quantity = count;

    const auto bytes = protocol::modbus::encodeRtuRequest(request);
    port_->write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<qint64>(bytes.size()));
    readPending_ = true;
    pendingReadTx_ = transaction;
    pendingSinceMs_ = QDateTime::currentMSecsSinceEpoch();
}

void ModbusRtuClient::poll()
{
    if (!connected_)
        return;

    // 版本读取等一次性请求优先：在途时不发轮询，避免串口响应互相错配。
    if (readPending_) {
        if (QDateTime::currentMSecsSinceEpoch() - pendingSinceMs_ < kRequestTimeoutMs)
            return;
        readPending_ = false;
        const std::uint16_t transaction = pendingReadTx_;
        pendingReadTx_ = 0;
        emit readCompleted(transaction, false, {});
    }

    if (pollPending_) {
        if (QDateTime::currentMSecsSinceEpoch() - pendingSinceMs_ < kRequestTimeoutMs)
            return;
        pollPending_ = false;
    }

    protocol::modbus::Request request;
    request.unitId = static_cast<std::uint8_t>(unitId_);
    request.function = protocol::modbus::FunctionCode::ReadInputRegisters;
    request.startAddress = 0;
    request.quantity = 6;

    const auto bytes = protocol::modbus::encodeRtuRequest(request);
    port_->write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<qint64>(bytes.size()));

    pollPending_ = true;
    pendingSinceMs_ = QDateTime::currentMSecsSinceEpoch();
}

void ModbusRtuClient::onReadyRead()
{
    buffer_.append(port_->readAll());

    while (true) {
        const auto* data = reinterpret_cast<const std::uint8_t*>(buffer_.constData());
        const std::size_t size = static_cast<std::size_t>(buffer_.size());
        const std::size_t frameLength = protocol::modbus::rtuFrameLength(data, size);
        if (frameLength == 0)
            break; // 半包

        protocol::modbus::Response response;
        const bool ok = protocol::modbus::decodeRtuResponse(
            data, frameLength, static_cast<std::uint8_t>(unitId_), response);

        buffer_.remove(0, static_cast<int>(frameLength));

        if (!ok) {
            // 校验失败通常意味着波特率/校验位不匹配，或总线上有干扰。
            emit failed(QStringLiteral("RTU 响应校验失败"));
            continue;
        }

        // 写单个寄存器的响应：功能码 0x06。
        if (response.function == protocol::modbus::FunctionCode::WriteSingleRegister) {
            const std::uint16_t transaction = pendingWriteTx_;
            pendingWriteTx_ = 0;
            emit writeCompleted(transaction, !response.isException,
                                response.isException ? response.exceptionCode : 0);
            continue;
        }

        // 读保持寄存器（FC03）的响应：一次性请求。
        if (readPending_ && response.function
                == protocol::modbus::FunctionCode::ReadHoldingRegisters) {
            const std::uint16_t transaction = pendingReadTx_;
            readPending_ = false;
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

        pollPending_ = false;

        if (response.isException) {
            emit failed(QStringLiteral("RTU 异常码 %1")
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

} // namespace aegis::infra
