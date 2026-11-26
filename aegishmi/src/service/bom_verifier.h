#pragma once // 头文件保护

#include "core/station_config.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace aegis::storage {
class SystemRepository;
}

namespace aegis::service {

// 一次 BOM 核对的结果。
struct BomCheckResult {
    QString materialCode; // 扫描到的料号
    QString expected;     // MES 返回的期望料号
    bool match = false;   // 料号是否一致
    bool ok = false;      // 查询本身是否成功（网络/HTTP/JSON）
    QString error;        // 查询失败原因
};

// =============================================================================
// BomVerifier —— 工位侧物料防错（扫码 + MES 只读查询 + 本地锁站）
//
// 边界：BOM 维护仍在 MES，本类只做"查询 + 判定 + 锁站"，不上传/修改物料数据。
//
// 判定规则：
//   - match=true         → 放行（写审计）
//   - match=false        → 锁定工位（需 supervisorUnlock 解锁，写审计）
//   - MES 不可用/响应异常 → 按 station.json 的 mes.failOpen：
//                            true（默认）提示后放行；false 锁定。
// =============================================================================
class BomVerifier : public QObject {
    Q_OBJECT

public:
    // audit 可为空：界面场景用 auditRequested 信号交给 AuthService 写审计；
    // 单元测试可直接注入 SystemRepository 验证落库。
    BomVerifier(const core::StationConfig& config,
                storage::SystemRepository* audit = nullptr,
                QObject* parent = nullptr);
    ~BomVerifier() override;

    // 是否启用（配置了 bomCheckPath）。
    bool enabled() const { return !config_.mesBomCheckPath.isEmpty(); }
    bool locked() const { return locked_; }
    QString lockReason() const { return lockReason_; }

    // 核对料号（异步，结果通过 verified 通知）。materialCode 为空直接判失败。
    void verify(const QString& workOrder, const QString& product,
                const QString& materialCode, const QString& operatorName);
    // 班组长确认后解锁（写审计）。
    void supervisorUnlock(const QString& supervisor);

signals:
    void verified(const aegis::service::BomCheckResult& result);
    void lockChanged(bool locked, const QString& reason);
    void logMessage(const QString& message);
    // 审计事件（界面接到 AuthService::audit）。
    void auditRequested(const QString& action, const QString& target,
                        const QString& result, const QString& detail);

private:
    void onReplyFinished(QNetworkReply* reply);
    void setLocked(bool locked, const QString& reason);
    void audit(const QString& result, const QString& detail);

    core::StationConfig config_;
    storage::SystemRepository* auditRepository_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;

    QString materialCode_;
    QString operatorName_;
    QString expected_;
    BomCheckResult lastResult_;
    bool locked_ = false;
    QString lockReason_;
};

} // namespace aegis::service

Q_DECLARE_METATYPE(aegis::service::BomCheckResult)
