#pragma once // 头文件保护

#include "service/device_summary.h"
#include "storage/test_repository.h" // SummaryRow

#include <QObject>
#include <QString>
#include <QThread>
#include <QVector>

class QTimer;

namespace aegis::storage {

// =============================================================================
// SummaryWriter —— 波形摘要的异步批量写入器
//
// 为什么单独开线程：
//   8 台设备 × 32 通道 = 每秒 256 行摘要。逐行写会频繁 fsync，
//   放在 UI 线程会每秒抖一下；这里攒批（默认 500 行或 500ms）后单事务提交。
//
// 线程模型：
//   - 本对象被 moveToThread(thread_)；
//   - enqueue() 可从任意线程调用，内部转成排队调用到写线程；
//   - 数据库连接在写线程内创建（Qt SQL 要求连接与使用线程一致）。
//
// 注意：本对象必须"无父对象"创建，否则 moveToThread 会失败（Qt 不允许带父对象迁移线程）。
// =============================================================================
class SummaryWriter : public QObject {
    Q_OBJECT

public:
    explicit SummaryWriter(QObject* parent = nullptr);
    ~SummaryWriter() override;

    // 启动写线程并打开数据库（与主界面共用同一个 aegis.db）。
    void start(const QString& dbPath);
    // 刷完剩余数据并退出写线程。
    void stop();

    // 入队一秒的摘要；deviceId 来自 fleet 配置。
    void enqueue(const QString& deviceId, const aegis::service::DeviceSummary& summary);

signals:
    // 一次批量提交完成（行数），便于统计/日志。
    void written(int rows);

private:
    void appendRows(const QString& deviceId, const aegis::service::DeviceSummary& summary);
    void flush();

    QThread thread_;
    TestRepository* repo_ = nullptr; // 仅在写线程访问
    QTimer* timer_ = nullptr;        // 仅在写线程访问
    QVector<SummaryRow> pending_;    // 仅在写线程访问
    bool started_ = false;
};

} // namespace aegis::storage
