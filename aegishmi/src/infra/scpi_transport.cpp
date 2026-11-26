#include "infra/scpi_transport.h"

#include <QTcpSocket>

namespace aegis::infra {

TcpScpiTransport::TcpScpiTransport(QObject* parent)
    : IScpiTransport(parent)
{
    socket_ = new QTcpSocket(this);
    connect(socket_, &QTcpSocket::connected, this, &IScpiTransport::opened);
    connect(socket_, &QTcpSocket::disconnected, this, &IScpiTransport::closed);
    connect(socket_, &QTcpSocket::readyRead, this, [this] {
        emit dataReady(socket_->readAll());
    });
    connect(socket_, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { emit failed(socket_->errorString()); });
}

void TcpScpiTransport::open(const QString& address, quint16 port)
{
    socket_->connectToHost(address, port);
}

void TcpScpiTransport::close()
{
    if (socket_->state() != QAbstractSocket::UnconnectedState)
        socket_->abort();
}

bool TcpScpiTransport::isOpen() const
{
    return socket_->state() == QAbstractSocket::ConnectedState;
}

void TcpScpiTransport::write(const QByteArray& data)
{
    socket_->write(data);
}

QString TcpScpiTransport::errorString() const
{
    return socket_->errorString();
}

QString TcpScpiTransport::transportName() const
{
    return QStringLiteral("tcp");
}

} // namespace aegis::infra
