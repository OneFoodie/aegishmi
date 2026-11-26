// Qt Test 框架
#include <QtTest>

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "infra/scpi_client.h"
#include "infra/scpi_transport.h"

using namespace aegis::infra;

namespace {

// =============================================================================
// MockScpiServer —— 极简 SCPI 仪器：按行接收，查询命令回一行文本
// =============================================================================
class MockScpiServer : public QObject {
    Q_OBJECT

public:
    explicit MockScpiServer(QObject* parent = nullptr)
        : QObject(parent)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = server_.nextPendingConnection()) {
                sockets_.append(socket);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    QByteArray& buffer = buffers_[socket];
                    buffer.append(socket->readAll());
                    while (true) {
                        const int pos = buffer.indexOf('\n');
                        if (pos < 0)
                            break;
                        const QString command =
                            QString::fromLatin1(buffer.left(pos)).trimmed();
                        buffer.remove(0, pos + 1);
                        handle(socket, command);
                    }
                });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
                    buffers_.remove(socket);
                    sockets_.removeAll(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    // 析构时先把已连接的 socket 从 this 上断开：QObject 析构会删除子对象，
    // 若此时回调仍指向本对象，会访问到已析构的成员（buffers_）而崩溃。
    ~MockScpiServer() override
    {
        const QList<QTcpSocket*> sockets = sockets_;
        for (QTcpSocket* socket : sockets) {
            socket->disconnect(this);
            socket->abort();
            delete socket;
        }
        sockets_.clear();
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server_.serverPort(); }

    void setReply(const QByteArray& reply) { reply_ = reply; }
    void setSilent(bool silent) { silent_ = silent; }
    QStringList received() const { return received_; }

private:
    void handle(QTcpSocket* socket, const QString& command)
    {
        received_.append(command);
        if (silent_)
            return;
        if (command.endsWith(QLatin1Char('?')))
            socket->write(reply_ + '\n');
    }

    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
    QList<QTcpSocket*> sockets_;
    QByteArray reply_ = QByteArrayLiteral("5.000000E+00");
    bool silent_ = false;
    QStringList received_;
};

// =============================================================================
// MockScpiTransport —— 不外接硬件的传输层，用于验证 GPIB/USBTMC 适配路径
// =============================================================================
class MockScpiTransport final : public IScpiTransport {
    Q_OBJECT

public:
    explicit MockScpiTransport(QObject* parent = nullptr)
        : IScpiTransport(parent)
    {
    }

    void open(const QString& address, quint16) override
    {
        address_ = address;
        open_ = true;
        emit opened();
    }
    void close() override
    {
        if (open_) {
            open_ = false;
            emit closed();
        }
    }
    bool isOpen() const override { return open_; }
    void write(const QByteArray& data) override
    {
        written_.append(data);
        if (data.trimmed() == QByteArrayLiteral("*IDN?"))
            emit dataReady(QByteArrayLiteral("AegisSim,DMM,0001,1.0\n"));
    }
    QString errorString() const override { return {}; }
    QString transportName() const override { return QStringLiteral("mock"); }

    QString address() const { return address_; }
    QStringList written() const { return written_; }

private:
    bool open_ = false;
    QString address_;
    QStringList written_;
};

} // namespace

// =============================================================================
// TstScpi —— SCPI 仪器客户端（SCPI-RAW over TCP）
// =============================================================================
class TstScpi : public QObject {
    Q_OBJECT

private slots:
    void queryReturnsResponse();
    void writeOnlyCompletes();
    void queryTimesOutWithoutResponse();
    void pollingEmitsMeasurement();
    void injectedTransportReusesScpiLogic();
};

void TstScpi::queryReturnsResponse()
{
    MockScpiServer server;
    QVERIFY(server.listen());

    ScpiClient client;
    QSignalSpy opened(&client, &ScpiClient::opened);
    client.open(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(opened.wait(3000));

    QSignalSpy completed(&client, &ScpiClient::queryCompleted);
    client.query(7, QByteArrayLiteral("MEAS:VOLT:DC?"), 1000);
    QVERIFY(completed.wait(3000));

    const QList<QVariant> args = completed.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(7));
    QCOMPARE(args.at(1).toBool(), true);
    QCOMPARE(args.at(2).toString(), QStringLiteral("5.000000E+00"));
    QCOMPARE(server.received().last(), QStringLiteral("MEAS:VOLT:DC?"));
}

void TstScpi::writeOnlyCompletes()
{
    MockScpiServer server;
    QVERIFY(server.listen());

    ScpiClient client;
    QSignalSpy opened(&client, &ScpiClient::opened);
    client.open(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(opened.wait(3000));

    QSignalSpy completed(&client, &ScpiClient::queryCompleted);
    client.writeCommand(3, QByteArrayLiteral("SOUR:VOLT 5.000"));
    // 只写命令不等待仪器响应，客户端会在写入后立即确认（同线程下是同步发出的）
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);

    const QList<QVariant> args = completed.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(3));
    // 只写命令由客户端直接确认，不依赖仪器响应
    QCOMPARE(args.at(1).toBool(), true);

    // 命令确实发到了服务端（\n 由客户端补齐）
    QTRY_COMPARE_WITH_TIMEOUT(server.received().size(), 1, 3000);
    QCOMPARE(server.received().first(), QStringLiteral("SOUR:VOLT 5.000"));
}

void TstScpi::queryTimesOutWithoutResponse()
{
    MockScpiServer server;
    server.setSilent(true);
    QVERIFY(server.listen());

    ScpiClient client;
    QSignalSpy opened(&client, &ScpiClient::opened);
    client.open(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(opened.wait(3000));

    QSignalSpy completed(&client, &ScpiClient::queryCompleted);
    client.query(9, QByteArrayLiteral("*IDN?"), 150);
    QVERIFY(completed.wait(3000));

    const QList<QVariant> args = completed.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(9));
    QCOMPARE(args.at(1).toBool(), false);
}

void TstScpi::pollingEmitsMeasurement()
{
    MockScpiServer server;
    QVERIFY(server.listen());

    ScpiClient client;
    QSignalSpy opened(&client, &ScpiClient::opened);
    client.setPolling(QStringLiteral("MEAS:VOLT:DC?"), 50);
    client.open(QStringLiteral("127.0.0.1"), server.port());
    QVERIFY(opened.wait(3000));

    QSignalSpy measured(&client, &ScpiClient::measurement);
    QTRY_VERIFY_WITH_TIMEOUT(measured.count() > 0, 3000);
    QVERIFY(qAbs(measured.first().at(0).toDouble() - 5.0) < 1e-6);
}

// 自定义传输层（模拟 GPIB/USBTMC 适配）：SCPI 排队/查询逻辑保持不变。
void TstScpi::injectedTransportReusesScpiLogic()
{
    auto* transport = new MockScpiTransport();
    MockScpiTransport* raw = transport;
    ScpiClient client(transport);

    QSignalSpy opened(&client, &ScpiClient::opened);
    client.open(QStringLiteral("GPIB0::22::INSTR"), 0);
    // mock 传输层在同一线程内同步发出 opened，用 QTRY 兼容同步/异步两种时序。
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 1000);
    QCOMPARE(raw->address(), QStringLiteral("GPIB0::22::INSTR"));

    QSignalSpy completed(&client, &ScpiClient::queryCompleted);
    client.query(5, QByteArrayLiteral("*IDN?"), 500);
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 1000);

    const QList<QVariant> args = completed.takeFirst();
    QCOMPARE(args.at(0).toUInt(), quint16(5));
    QCOMPARE(args.at(1).toBool(), true);
    QCOMPARE(args.at(2).toString(), QStringLiteral("AegisSim,DMM,0001,1.0"));
    QCOMPARE(raw->written().value(0), QByteArrayLiteral("*IDN?\n"));
}

QTEST_GUILESS_MAIN(TstScpi)
#include "tst_scpi.moc"
