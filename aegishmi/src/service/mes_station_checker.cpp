#include "service/mes_station_checker.h"

#include "storage/system_repository.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace aegis::service {

namespace {

// stationCheckPath 可以是完整 URL，也可以是相对 mesUrl 的路径。
QUrl resolveEndpoint(const core::StationConfig& config)
{
    const QUrl path(config.mesStationCheckPath);
    if (path.isValid() && !path.scheme().isEmpty())
        return path;

    const QUrl base(config.mesUrl);
    if (!base.isValid())
        return path;
    return base.resolved(path);
}

} // namespace

MesStationChecker::MesStationChecker(const core::StationConfig& config,
                                     storage::SystemRepository* audit,
                                     QObject* parent)
    : QObject(parent)
    , config_(config)
    , auditRepository_(audit)
    , network_(new QNetworkAccessManager(this))
{
}

MesStationChecker::~MesStationChecker() = default;

void MesStationChecker::check(const QString& workOrder, const QString& sn,
                              const QString& product, const QString& operatorName,
                              const QString& fixtureId)
{
    if (reply_ != nullptr) {
        emit logMessage(QStringLiteral("过站校验正在进行，请稍候"));
        return;
    }

    workOrder_ = workOrder.trimmed();
    sn_ = sn.trimmed();
    operatorName_ = operatorName;

    if (!enabled()) {
        StationCheckResult skipped;
        skipped.workOrder = workOrder_;
        skipped.sn = sn_;
        skipped.ok = true;
        skipped.passed = true;
        emit checked(skipped);
        return;
    }

    QNetworkRequest request(resolveEndpoint(config_));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(config_.mesTimeoutMs);

    QJsonObject body;
    body.insert(QStringLiteral("workOrder"), workOrder_);
    body.insert(QStringLiteral("sn"), sn_);
    body.insert(QStringLiteral("product"), product);
    body.insert(QStringLiteral("stationId"), config_.stationId);
    body.insert(QStringLiteral("lineId"), config_.lineId);
    body.insert(QStringLiteral("fixtureId"), fixtureId);
    body.insert(QStringLiteral("operator"), operatorName_);

    emit logMessage(QStringLiteral("过站校验：SN %1（工单 %2）").arg(sn_, workOrder_));
    reply_ = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply_, &QNetworkReply::finished, this,
            [this] { onReplyFinished(reply_); });
}

void MesStationChecker::onReplyFinished(QNetworkReply* reply)
{
    if (reply == nullptr)
        return;

    StationCheckResult result;
    result.workOrder = workOrder_;
    result.sn = sn_;

    const QVariant statusAttribute =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int httpStatus = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
    const bool httpOk = reply->error() == QNetworkReply::NoError
                     && httpStatus >= 200 && httpStatus < 300;

    if (httpOk) {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (document.isObject()) {
            const QJsonObject object = document.object();
            if (object.contains(QStringLiteral("ok"))) {
                result.ok = true;
                result.passed = object.value(QStringLiteral("ok")).toBool(false);
            } else if (object.contains(QStringLiteral("pass"))) {
                result.ok = true;
                result.passed = object.value(QStringLiteral("pass")).toBool(false);
            } else if (object.contains(QStringLiteral("result"))) {
                result.ok = true;
                result.passed = object.value(QStringLiteral("result"))
                                    .toString()
                                    .compare(QStringLiteral("PASS"), Qt::CaseInsensitive) == 0;
            } else {
                result.message = QStringLiteral("MES 响应缺少 ok/pass/result 字段");
            }
            const QString message = object.value(QStringLiteral("message")).toString().trimmed();
            const QString reason = object.value(QStringLiteral("reason")).toString().trimmed();
            if (!message.isEmpty())
                result.message = message;
            else if (!reason.isEmpty())
                result.message = reason;
        } else {
            result.message = QStringLiteral("MES 返回的内容不是 JSON 对象");
        }
    } else {
        result.message = reply->errorString();
        if (httpStatus > 0)
            result.message += QStringLiteral("（HTTP %1）").arg(httpStatus);
    }

    reply_ = nullptr;
    reply->deleteLater();

    if (result.ok && result.passed) {
        audit(QStringLiteral("通过"), result.message);
        emit logMessage(QStringLiteral("过站校验通过：SN %1").arg(result.sn));
    } else if (result.ok) {
        const QString reason = result.message.isEmpty()
            ? QStringLiteral("MES 未允许该 SN 过站")
            : result.message;
        result.message = reason;
        audit(QStringLiteral("拦截"), reason);
        emit logMessage(QStringLiteral("过站校验拦截：%1").arg(reason));
    } else if (config_.mesFailOpen) {
        // 与物料核对一致的默认策略：MES 不可用时放行，避免停线。
        audit(QStringLiteral("降级放行"), result.message);
        emit logMessage(QStringLiteral("MES 不可用，按 failOpen 放行：%1").arg(result.message));
    } else {
        audit(QStringLiteral("拦截"),
              QStringLiteral("MES 不可用：%1").arg(result.message));
        emit logMessage(QStringLiteral("MES 不可用，严格模式拦截：%1").arg(result.message));
    }

    emit checked(result);
}

void MesStationChecker::audit(const QString& result, const QString& detail)
{
    const QString action = QStringLiteral("过站校验");
    const QString target = sn_.isEmpty() ? workOrder_ : sn_;

    if (auditRepository_ != nullptr && auditRepository_->isOpen()) {
        storage::AuditEntry entry;
        entry.timestampMs = QDateTime::currentMSecsSinceEpoch();
        entry.username = operatorName_.isEmpty() ? QStringLiteral("—") : operatorName_;
        entry.action = action;
        entry.target = target;
        entry.result = result;
        entry.detail = detail;
        auditRepository_->appendAudit(entry);
    }

    emit auditRequested(action, target, result, detail);
}

} // namespace aegis::service
