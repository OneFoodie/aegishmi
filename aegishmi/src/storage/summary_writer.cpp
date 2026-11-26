#include "storage/summary_writer.h"

#include <QDateTime>
#include <QMetaObject>
#include <QTimer>

namespace aegis::storage {

namespace {
constexpr int kFlushIntervalMs = 500; // 最多攒 500ms 就写一次
constexpr int kFlushRowLimit = 500;   // 或攒到 500 行就立即写
} // namespace

SummaryWriter::SummaryWriter(QObject* parent)
    : QObject(parent)
{
    // 对象整体搬到写线程；enqueue 通过排队调用进入该线程。
    moveToThread(&thread_);
}

SummaryWriter::~SummaryWriter()
{
    stop();
}

void SummaryWriter::start(const QString& dbPath)
{
    if (started_)
        return;
    started_ = true;
    thread_.start();

    QMetaObject::invokeMethod(this, [this, dbPath] {
        repo_ = new TestRepository();
        QString error;
        if (!repo_->open(dbPath, &error)) {
            // 打不开就禁用写库，不影响采集/显示。
            delete repo_;
            repo_ = nullptr;
        } else {
            // 保留策略：只留最近 7 天的秒级摘要。
            const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - 7 * 24 * 3600;
            repo_->purgeSummariesBefore(cutoff);
        }

        timer_ = new QTimer(this);
        timer_->setInterval(kFlushIntervalMs);
        connect(timer_, &QTimer::timeout, this, &SummaryWriter::flush);
        timer_->start();
    }, Qt::QueuedConnection);
}

void SummaryWriter::stop()
{
    if (!started_)
        return;

    auto cleanup = [this] {
        if (timer_ != nullptr) {
            timer_->stop();
            delete timer_;
            timer_ = nullptr;
        }
        flush();
        if (repo_ != nullptr) {
            delete repo_;
            repo_ = nullptr;
        }
    };

    // 阻塞式调用：先刷完剩余数据、释放数据库连接，再退出线程。
    // 如果对象与调用者在同一线程（例如创建时带了父对象导致 moveToThread 失败），
    // BlockingQueuedConnection 会死锁；这里直接执行。
    if (QThread::currentThread() == thread()) {
        cleanup();
    } else {
        QMetaObject::invokeMethod(this, cleanup, Qt::BlockingQueuedConnection);
    }

    thread_.quit();
    thread_.wait(3000);
    started_ = false;
}

void SummaryWriter::enqueue(const QString& deviceId,
                            const aegis::service::DeviceSummary& summary)
{
    if (!started_)
        return;

    QMetaObject::invokeMethod(this, [this, deviceId, summary] {
        appendRows(deviceId, summary);
        if (pending_.size() >= kFlushRowLimit)
            flush();
    }, Qt::QueuedConnection);
}

void SummaryWriter::appendRows(const QString& deviceId,
                               const aegis::service::DeviceSummary& summary)
{
    for (int ch = 0; ch < summary.channels.size(); ++ch) {
        const aegis::service::ChannelSummary& channel = summary.channels.at(ch);
        if (channel.sampleCount <= 0)
            continue;

        SummaryRow row;
        row.deviceId = deviceId;
        row.channel = ch;
        row.epochSec = summary.epochSec;
        row.minValue = channel.minValue;
        row.maxValue = channel.maxValue;
        row.avgValue = channel.avgValue;
        row.sampleCount = channel.sampleCount;
        pending_.append(row);
    }
}

void SummaryWriter::flush()
{
    if (repo_ == nullptr || pending_.isEmpty())
        return;

    const int rows = pending_.size();
    QString error;
    if (repo_->saveSummaryBatch(pending_, &error))
        emit written(rows);

    pending_.clear();
}

} // namespace aegis::storage
