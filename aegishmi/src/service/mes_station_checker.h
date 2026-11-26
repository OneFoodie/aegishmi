#pragma once // 头文件保护

#include "core/station_config.h"

#include <QMetaType>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace aegis::storage {
class SystemRepository;
}

namespace aegis::service {

// 一次 MES 过站/工单校验的结果。
struct StationCheckResult {
    QString sn;
    QString workOrder;
    bool ok = false;      // 查询本身是否成功（网络/HTTP/JSON）
    bool passed = false;  // MES 是否允许该 SN 过站
    QString message;      // MES 返回的说明 / 拦截原因
};

// =============================================================================
// MesStationChecker —— 测试开始前的 MES 过站 / 工单校验
//
// 与 BomVerifier 的分工：BOM 核对回答"料对不对"，本类回答"这个 SN 现在
// 能不能在这个工位测"（工单状态、工序、是否已测过、是否被报废/冻结等）。
//
// 判定规则：
//   - passed=true            → 放行（写审计）
//   - passed=false           → 拦截（不启动测试，写审计）
//   - MES 不可用/响应异常     → 按 mes.failOpen：true 提示后放行，false 拦截
//
// 未配置 mes.stationCheckPath 时 enabled()==false，行为与原来完全一致。
// =============================================================================
class MesStationChecker : public QObject {
    Q_OBJECT

public:
    // audit 可为空：界面场景用 auditRequested 交给 AuthService 落审计。
    MesStationChecker(const core::StationConfig& config,
                      storage::SystemRepository* audit = nullptr,
                      QObject* parent = nullptr);
    ~MesStationChecker() override;

    bool enabled() const { return !config_.mesStationCheckPath.isEmpty(); }

    // 异步查询，结果通过 checked 通知；并发调用会被忽略（保留在途请求）。
    void check(const QString& workOrder, const QString& sn, const QString& product,
               const QString& operatorName, const QString& fixtureId);

signals:
    void checked(const aegis::service::StationCheckResult& result);
    void logMessage(const QString& message);
    void auditRequested(const QString& action, const QString& target,
                        const QString& result, const QString& detail);

private:
    void onReplyFinished(QNetworkReply* reply);
    void audit(const QString& result, const QString& detail);

    core::StationConfig config_;
    storage::SystemRepository* auditRepository_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;

    QString workOrder_;
    QString sn_;
    QString operatorName_;
};

} // namespace aegis::service

Q_DECLARE_METATYPE(aegis::service::StationCheckResult)
