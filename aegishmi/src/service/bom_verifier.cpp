#include "service/bom_verifier.h"

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

// bomCheckPath 可以是完整 URL，也可以是相对 mesUrl 的路径（建议以 / 开头）。
QUrl resolveEndpoint(const core::StationConfig& config)
{
    const QUrl path(config.mesBomCheckPath);
    if (path.isValid() && !path.scheme().isEmpty())
        return path;

    const QUrl base(config.mesUrl);
    if (!base.isValid())
        return path;
    return base.resolved(path);
}

} // namespace

BomVerifier::BomVerifier(const core::StationConfig& config,
                         storage::SystemRepository* audit, QObject* parent)
    : QObject(parent)
    , config_(config)
    , auditRepository_(audit)
    , network_(new QNetworkAccessManager(this))
{
}

BomVerifier::~BomVerifier() = default;

void BomVerifier::verify(const QString& workOrder, const QString& product,
                         const QString& materialCode, const QString& operatorName)
{
    if (reply_ != nullptr) {
        emit logMessage(QStringLiteral("料号核对正在进行，请稍候"));
        return;
    }

    materialCode_ = materialCode.trimmed();
    operatorName_ = operatorName;

    if (!enabled()) {
        // 未配置核对接口：不做拦截，保持原有流程。
        BomCheckResult skipped;
        skipped.materialCode = materialCode_;
        emit verified(skipped);
        return;
    }

    if (materialCode_.isEmpty()) {
        BomCheckResult result;
        result.error = QStringLiteral("请先扫描/输入料号");
        lastResult_ = result;
        emit verified(result);
        return;
    }

    QNetworkRequest request(resolveEndpoint(config_));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(config_.mesTimeoutMs);

    QJsonObject body;
    body.insert(QStringLiteral("workOrder"), workOrder);
    body.insert(QStringLiteral("product"), product);
    body.insert(QStringLiteral("materialCode"), materialCode_);

    emit logMessage(QStringLiteral("料号核对：%1（工单 %2）").arg(materialCode_, workOrder));
    reply_ = network_->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply_, &QNetworkReply::finished, this,
            [this] { onReplyFinished(reply_); });
}

void BomVerifier::onReplyFinished(QNetworkReply* reply)
{
    if (reply == nullptr)
        return;

    BomCheckResult result;
    result.materialCode = materialCode_;

    const QVariant statusAttribute =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int httpStatus = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
    const bool httpOk = reply->error() == QNetworkReply::NoError
                     && httpStatus >= 200 && httpStatus < 300;

    if (httpOk) {
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        if (document.isObject()) {
            const QJsonObject object = document.object();
            result.ok = true;
            result.match = object.value(QStringLiteral("match")).toBool(false);
            result.expected = object.value(QStringLiteral("expected")).toString();
        } else {
            result.error = QStringLiteral("MES 返回的内容不是 JSON 对象");
        }
    } else {
        result.error = reply->errorString();
        if (httpStatus > 0)
            result.error += QStringLiteral("（HTTP %1）").arg(httpStatus);
    }

    reply_ = nullptr;
    reply->deleteLater();
    expected_ = result.expected;
    lastResult_ = result;

    if (result.ok && result.match) {
        setLocked(false, QString());
        audit(QStringLiteral("成功"),
              QStringLiteral("料号=%1 期望=%2").arg(result.materialCode, expected_));
        emit logMessage(QStringLiteral("料号核对通过：%1").arg(result.materialCode));
    } else if (result.ok) {
        const QString reason = QStringLiteral("料号不符：扫描 %1，期望 %2")
                                   .arg(result.materialCode.isEmpty() ? QStringLiteral("—")
                                                                      : result.materialCode,
                                        result.expected.isEmpty() ? QStringLiteral("—")
                                                                  : result.expected);
        setLocked(true, reason);
        audit(QStringLiteral("锁定"),
              QStringLiteral("料号=%1 期望=%2").arg(result.materialCode, result.expected));
        emit logMessage(QStringLiteral("料号不符，工位已锁定：%1").arg(reason));
    } else if (config_.mesFailOpen) {
        // 默认策略：MES 不可用时提示后放行，避免停线。
        setLocked(false, QString());
        audit(QStringLiteral("降级放行"), result.error);
        emit logMessage(QStringLiteral("MES 不可用，按 failOpen 放行：%1").arg(result.error));
    } else {
        setLocked(true, QStringLiteral("MES 不可用：%1").arg(result.error));
        audit(QStringLiteral("锁定"), result.error);
        emit logMessage(QStringLiteral("MES 不可用，严格模式锁定工位：%1").arg(result.error));
    }

    emit verified(result);
}

void BomVerifier::supervisorUnlock(const QString& supervisor)
{
    if (!locked_)
        return;
    const QString reason = lockReason_;
    setLocked(false, QString());
    audit(QStringLiteral("解锁"),
          QStringLiteral("操作人=%1 原锁定原因=%2")
              .arg(supervisor.isEmpty() ? QStringLiteral("—") : supervisor, reason));
    emit logMessage(QStringLiteral("工位已由 %1 解锁").arg(supervisor));
}

void BomVerifier::setLocked(bool locked, const QString& reason)
{
    const bool changed = locked_ != locked || lockReason_ != reason;
    locked_ = locked;
    lockReason_ = locked ? reason : QString();
    if (changed)
        emit lockChanged(locked_, lockReason_);
}

void BomVerifier::audit(const QString& result, const QString& detail)
{
    const QString action = QStringLiteral("物料核对");
    const QString target = materialCode_;

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
