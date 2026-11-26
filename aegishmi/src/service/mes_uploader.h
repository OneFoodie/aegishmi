#pragma once // 头文件保护

#include "core/station_config.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace aegis::storage {
class TestRepository;
class SystemRepository;
}

namespace aegis::service {

// =============================================================================
// MesUploader —— 测试结果上传 MES（HTTP JSON）
//
// 设计要点：
//   - 测试结束时把报文写入本地 mes_outbox（PENDING），再由本类异步发送；
//   - 发送失败按指数退避重试（base、2×base、4×base…，上限 60s）；
//   - 达到最大尝试次数标记 DEAD，可在生产看板"重传失败项"；
//   - 程序重启后继续处理未完成的 PENDING 记录（断网补传）；
//   - 全部状态落库，界面可看到待传/失败数量。
//
// 线程模型：运行在 UI 线程，QNetworkAccessManager 异步收发，不阻塞界面。
// =============================================================================
class MesUploader : public QObject {
    Q_OBJECT

public:
    MesUploader(storage::TestRepository* repository,
                const core::StationConfig& config,
                storage::SystemRepository* audit = nullptr,
                QObject* parent = nullptr);

    // 启动轮询（配置未启用时什么也不做）。
    void start();
    // 停止轮询并中止在途请求（退出程序时调用）。
    void stop();

    bool enabled() const { return config_.mesEnabled && !config_.mesUrl.isEmpty(); }

    // 把一次运行加入上传队列。未启用 MES 时直接忽略。
    void enqueue(int runId);

    // 把 DEAD 记录重新放回队列并立即尝试（人工重传）。
    void retryDead();

    // 立即处理一次队列（不等定时器）。
    void kick() { processQueue(); }

signals:
    // 队列状态变化（界面刷新待传/失败数量）。
    void statusChanged();
    // 便于写入运行日志。
    void logMessage(const QString& message);

private slots:
    void processQueue();
    void onReplyFinished(QNetworkReply* reply);

private:
    void audit(const QString& result, const QString& detail);

    storage::TestRepository* repository_ = nullptr;
    storage::SystemRepository* auditRepository_ = nullptr;
    core::StationConfig config_;
    QNetworkAccessManager* network_ = nullptr;
    QTimer* timer_ = nullptr;
    QNetworkReply* reply_ = nullptr; // 在途请求
    int currentId_ = 0;
    int currentAttempts_ = 0;
};

} // namespace aegis::service
