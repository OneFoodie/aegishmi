#include "infra/scpi_client.h"

#include "infra/scpi_transport.h"

#include <QTimer>

#include <algorithm>
#include <cmath>

namespace aegis::infra {

namespace {
constexpr int kMaxReceiveBytes = 64 * 1024; // 单次缓冲上限，异常数据直接丢弃
}

ScpiClient::ScpiClient(QObject* parent)
    : ScpiClient(new TcpScpiTransport(), parent)
{
}

ScpiClient::ScpiClient(IScpiTransport* transport, QObject* parent)
    : QObject(parent)
    , transport_(transport)
{
    if (transport_ != nullptr) {
        transport_->setParent(this);
        connect(transport_, &IScpiTransport::opened, this, &ScpiClient::onConnected);
        connect(transport_, &IScpiTransport::closed, this, &ScpiClient::onDisconnected);
        connect(transport_, &IScpiTransport::dataReady, this, &ScpiClient::onDataReady);
        connect(transport_, &IScpiTransport::failed, this, &ScpiClient::failed);
    }

    timeoutTimer_ = new QTimer(this);
    timeoutTimer_->setSingleShot(true);
    pollTimer_ = new QTimer(this);

    connect(timeoutTimer_, &QTimer::timeout, this, &ScpiClient::onTimeout);
    connect(pollTimer_, &QTimer::timeout, this, &ScpiClient::onPollTick);
}

ScpiClient::~ScpiClient() = default;

void ScpiClient::open(const QString& host, quint16 port)
{
    resetState();
    if (transport_ != nullptr)
        transport_->open(host, port);
}

void ScpiClient::close()
{
    resetState();
    if (transport_ != nullptr)
        transport_->close();
}

void ScpiClient::resetState()
{
    pollTimer_->stop();
    timeoutTimer_->stop();
    queue_.clear();
    current_ = Request{};
    busy_ = false;
    buffer_.clear();
}

void ScpiClient::setPolling(const QString& command, int intervalMs)
{
    pollCommand_ = command.trimmed();
    pollIntervalMs_ = std::clamp(intervalMs, 50, 5000);

    if (pollCommand_.isEmpty()) {
        pollTimer_->stop();
        return;
    }

    pollTimer_->setInterval(pollIntervalMs_);
    if (transport_ != nullptr && transport_->isOpen())
        pollTimer_->start();
}

void ScpiClient::onConnected()
{
    emit opened();
    if (!pollCommand_.isEmpty())
        pollTimer_->start();
    pump();
}

void ScpiClient::onDisconnected()
{
    const bool pending = busy_;
    const Request interrupted = current_;
    resetState();
    if (pending && !interrupted.poll)
        emit queryCompleted(interrupted.transaction, false, QString());
    emit closed();
}

void ScpiClient::enqueue(const Request& request)
{
    if (transport_ == nullptr || !transport_->isOpen()) {
        if (!request.poll)
            emit queryCompleted(request.transaction, false, QString());
        return;
    }
    queue_.push_back(request);
    pump();
}

void ScpiClient::pump()
{
    if (busy_ || queue_.empty() || transport_ == nullptr || !transport_->isOpen())
        return;

    current_ = queue_.front();
    queue_.pop_front();
    busy_ = true;
    buffer_.clear();

    QByteArray line = current_.command;
    if (!line.endsWith('\n'))
        line.append('\n');
    transport_->write(line);

    if (current_.expectResponse) {
        timeoutTimer_->start(std::max(50, current_.timeoutMs));
        return;
    }

    // 只写命令：写出去即可认为完成，继续处理下一条。
    const Request finished = current_;
    busy_ = false;
    emit queryCompleted(finished.transaction, true, QString());
    pump();
}

void ScpiClient::writeCommand(quint16 transaction, const QByteArray& command)
{
    Request request;
    request.transaction = transaction;
    request.command = command;
    request.expectResponse = false;
    enqueue(request);
}

void ScpiClient::query(quint16 transaction, const QByteArray& command, int timeoutMs)
{
    Request request;
    request.transaction = transaction;
    request.command = command;
    request.expectResponse = true;
    request.timeoutMs = std::max(50, timeoutMs);
    enqueue(request);
}

void ScpiClient::onPollTick()
{
    if (pollCommand_.isEmpty())
        return;

    Request request;
    request.transaction = 0; // 轮询不占用命令事务号
    request.command = pollCommand_.toLatin1();
    request.expectResponse = true;
    request.timeoutMs = std::max(500, pollIntervalMs_ * 3);
    request.poll = true;
    enqueue(request);
}

void ScpiClient::onDataReady(const QByteArray& data)
{
    buffer_.append(data);
    if (buffer_.size() > kMaxReceiveBytes) {
        buffer_.clear();
        emit failed(QStringLiteral("SCPI 响应超过缓冲上限，已丢弃"));
        return;
    }

    // 仪器一次可能返回多行，逐行处理。
    while (true) {
        const int pos = buffer_.indexOf('\n');
        if (pos < 0)
            break;

        const QString line = QString::fromLatin1(buffer_.left(pos)).trimmed();
        buffer_.remove(0, pos + 1);

        if (!busy_)
            continue; // 没有在等响应：丢弃多余的输出

        const Request finished = current_;
        busy_ = false;
        timeoutTimer_->stop();

        if (finished.poll) {
            // 取第一个字段并解析成数值；解析失败视为本次无效。
            bool ok = false;
            const double value =
                line.section(QLatin1Char(','), 0, 0).trimmed().toDouble(&ok);
            if (ok && std::isfinite(value))
                emit measurement(value);
        } else if (finished.expectResponse) {
            emit queryCompleted(finished.transaction, true, line);
        }

        pump();
    }
}

void ScpiClient::onTimeout()
{
    if (!busy_)
        return;

    const Request finished = current_;
    busy_ = false;
    if (!finished.poll)
        emit queryCompleted(finished.transaction, false, QString());
    pump();
}

} // namespace aegis::infra
