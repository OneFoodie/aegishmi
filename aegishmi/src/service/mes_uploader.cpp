#include "service/mes_uploader.h"

#include "storage/test_repository.h"
#include "storage/system_repository.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace aegis::service {

namespace {
constexpr int kPollIntervalMs = 2000;   // 队列轮询周期
constexpr int kMaxBackoffMs = 60000;    // 单次退避上限
} // namespace

MesUploader::MesUploader(storage::TestRepository* repository,
                         const core::StationConfig& config,
                         storage::SystemRepository* audit, QObject* parent)
    : QObject(parent)
    , repository_(repository)
    , auditRepository_(audit)
    , config_(config)
{
    network_ = new QNetworkAccessManager(this);

    timer_ = new QTimer(this);
    timer_->setInterval(kPollIntervalMs);
    connect(timer_, &QTimer::timeout, this, &MesUploader::processQueue);
}

void MesUploader::start()
{
    if (!enabled())
        return;
    timer_->start();
    processQueue();
}

void MesUploader::stop()
{
    timer_->stop();

    // 先断开 finished，避免 abort 同步回调到 onReplyFinished 后重复处理。
    QNetworkReply* pending = reply_;
    reply_ = nullptr;
    currentId_ = 0;
    currentAttempts_ = 0;
    if (pending != nullptr) {
        pending->disconnect(this);
        pending->abort();
        pending->deleteLater();
    }
}

void MesUploader::enqueue(int runId)
{
    if (!enabled() || repository_ == nullptr || !repository_->isOpen())
        return;

    const QByteArray payload = repository_->buildRunPayload(runId);
    if (payload.isEmpty()) {
        emit logMessage(QStringLiteral("MES：生成运行 #%1 报文失败，未入队").arg(runId));
        return;
    }

    QString error;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!repository_->enqueueUpload(runId, payload, now, &error)) {
        emit logMessage(QStringLiteral("MES：入队失败 %1").arg(error));
        return;
    }

    emit statusChanged();
    processQueue();
}

void MesUploader::retryDead()
{
    if (!enabled() || repository_ == nullptr)
        return;
    repository_->requeueDeadUploads(QDateTime::currentMSecsSinceEpoch());
    emit statusChanged();
    processQueue();
}

void MesUploader::processQueue()
{
    if (!enabled() || repository_ == nullptr || !repository_->isOpen())
        return;
    if (reply_ != nullptr)
        return; // 已有在途请求，等回调

    const QVector<storage::MesUploadItem> items =
        repository_->dueUploads(QDateTime::currentMSecsSinceEpoch(), 1);
    if (items.isEmpty())
        return;

    const storage::MesUploadItem item = items.first();
    currentId_ = item.id;
    currentAttempts_ = item.attempts;

    QNetworkRequest request(QUrl(config_.mesUrl));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setTransferTimeout(config_.mesTimeoutMs);

    reply_ = network_->post(request, item.payload);
    connect(reply_, &QNetworkReply::finished, this,
            [this] { onReplyFinished(reply_); });
}

void MesUploader::onReplyFinished(QNetworkReply* reply)
{
    if (reply == nullptr)
        return;

    const QVariant statusAttribute =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int httpStatus = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
    const QByteArray body = reply->readAll();

    bool ok = reply->error() == QNetworkReply::NoError
           && httpStatus >= 200 && httpStatus < 300;
    QString error = reply->errorString();
    if (httpStatus > 0)
        error += QStringLiteral("（HTTP %1）").arg(httpStatus);

    // HTTP 2xx 之外看响应体：{"ok":false,"message":"..."} 属于业务失败，
    // 不能当作上传成功（真实 MES 常在 200 里返回业务错误）。
    if (ok && !body.trimmed().isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(body);
        if (document.isObject()) {
            const QJsonObject object = document.object();
            bool accepted = true;
            if (object.contains(QStringLiteral("ok")))
                accepted = object.value(QStringLiteral("ok")).toBool(true);
            else if (object.contains(QStringLiteral("success")))
                accepted = object.value(QStringLiteral("success")).toBool(true);

            if (!accepted) {
                ok = false;
                error = object.value(QStringLiteral("message")).toString().trimmed();
                if (error.isEmpty())
                    error = object.value(QStringLiteral("error")).toString().trimmed();
                if (error.isEmpty())
                    error = QStringLiteral("MES 返回业务失败");
            }
        }
    }

    const int id = currentId_;
    const int attempts = currentAttempts_ + 1;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (ok) {
        repository_->markUploaded(id, now);
        audit(QStringLiteral("成功"),
              QStringLiteral("运行 #%1 上传成功").arg(id));
        emit logMessage(QStringLiteral("MES：上传成功（记录 #%1）").arg(id));
    } else {
        if (attempts >= config_.mesMaxAttempts) {
            repository_->markUploadDead(id, attempts, error);
            audit(QStringLiteral("失败"),
                  QStringLiteral("运行 #%1 上传失败已达上限：%2").arg(id).arg(error));
            emit logMessage(QStringLiteral("MES：上传失败已达上限，转人工重传：%1").arg(error));
        } else {
            // 指数退避：base × 2^(attempts-1)，上限 60 秒。
            const int shift = std::min(attempts - 1, 5);
            const qint64 backoff = std::min<qint64>(
                static_cast<qint64>(config_.mesRetryBaseMs) << shift, kMaxBackoffMs);
            repository_->markUploadFailed(id, attempts, error, now + backoff);
            audit(QStringLiteral("重试"),
                  QStringLiteral("运行 #%1 第 %2 次失败：%3")
                      .arg(id)
                      .arg(attempts)
                      .arg(error));
            emit logMessage(QStringLiteral("MES：上传失败，%1 ms 后重试：%2")
                                .arg(backoff)
                                .arg(error));
        }
    }

    reply_ = nullptr;
    reply->deleteLater();
    currentId_ = 0;
    currentAttempts_ = 0;

    emit statusChanged();
    // 队列里可能还有其它记录，下一轮事件循环继续。
    QTimer::singleShot(0, this, &MesUploader::processQueue);
}

void MesUploader::audit(const QString& result, const QString& detail)
{
    if (auditRepository_ == nullptr || !auditRepository_->isOpen())
        return;

    storage::AuditEntry entry;
    entry.timestampMs = QDateTime::currentMSecsSinceEpoch();
    entry.username = QStringLiteral("系统");
    entry.action = QStringLiteral("MES上传");
    entry.target = QStringLiteral("运行#%1").arg(currentId_);
    entry.result = result;
    entry.detail = detail;
    auditRepository_->appendAudit(entry);
}

} // namespace aegis::service
