#pragma once // 头文件保护

#include <QByteArray>
#include <QObject>
#include <QString>

#include <deque>

class QTimer;

namespace aegis::infra {

class IScpiTransport;

// =============================================================================
// ScpiClient —— SCPI 客户端（默认 SCPI-RAW over TCP，端口 5025）
//
// 适用场景：支持 LAN 接口的台式仪器（万用表/电源/电子负载等）。
// 协议约定：
//   - 每条命令以 '\n' 结束；
//   - 查询命令（以 '?' 结尾）返回一行文本，以 '\n' 结束；
//   - 同一时刻只允许一个"在途"请求，其余排队，避免响应错配。
//
// 轮询：setPolling() 设置周期查询（如 MEAS:VOLT:DC?），连接建立后自动按
// 周期采集，解析出第一个数值并通过 measurement() 发出。
//
// 传输层：默认 TcpScpiTransport；也可注入 IScpiTransport 实现（GPIB/USBTMC
// 适配或测试 mock），SCPI 报文逻辑不变。
//
// 线程模型：与 TcpTransport 一致，对象应 moveToThread 到通信线程后使用。
// =============================================================================
class ScpiClient : public QObject {
    Q_OBJECT

public:
    explicit ScpiClient(QObject* parent = nullptr);
    // 注入自定义传输层（本对象接管所有权）。
    explicit ScpiClient(IScpiTransport* transport, QObject* parent = nullptr);
    ~ScpiClient() override;

public slots:
    // 连接仪器（异步，结果通过 opened/closed/failed 通知）。
    void open(const QString& host, quint16 port);
    void close();

    // 设置周期查询命令；command 为空表示停止轮询。
    void setPolling(const QString& command, int intervalMs);

    // 只写命令（不需要响应）。
    void writeCommand(quint16 transaction, const QByteArray& command);
    // 查询命令：收到一行响应后发 queryCompleted；超时发 ok=false。
    void query(quint16 transaction, const QByteArray& command, int timeoutMs);

signals:
    void opened();
    void closed();
    void failed(const QString& reason);
    void queryCompleted(quint16 transaction, bool ok, const QString& response);
    // 轮询查询解析出的数值（仪器原始单位）。
    void measurement(double value);

private slots:
    void onConnected();
    void onDisconnected();
    void onDataReady(const QByteArray& data);
    void onTimeout();
    void onPollTick();

private:
    struct Request {
        quint16 transaction = 0;
        QByteArray command;
        bool expectResponse = false;
        int timeoutMs = 1000;
        bool poll = false;
    };

    void enqueue(const Request& request);
    void pump();
    void resetState();

    IScpiTransport* transport_ = nullptr;
    QTimer* timeoutTimer_ = nullptr; // 单次超时
    QTimer* pollTimer_ = nullptr;    // 周期查询
    QByteArray buffer_;              // 接收缓冲（按行切分）
    std::deque<Request> queue_;      // 待发送请求
    Request current_;                // 在途请求
    bool busy_ = false;
    QString pollCommand_;
    int pollIntervalMs_ = 100;
};

} // namespace aegis::infra
