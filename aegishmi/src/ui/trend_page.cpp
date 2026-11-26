#include "ui/trend_page.h"

#include "protocol/frame.h" // kMaxChannels
#include "service/device_manager.h"
#include "storage/test_repository.h"
#include "ui/trend_view.h"

#include <QComboBox>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPair>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

namespace aegis::ui {

TrendPage::TrendPage(storage::TestRepository* repository, service::DeviceManager* manager,
                     QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , manager_(manager)
{
    buildUi();
    refresh();
}

void TrendPage::buildUi()
{
    auto* root = new QVBoxLayout(this);

    auto* controls = new QHBoxLayout();

    controls->addWidget(new QLabel(QStringLiteral("设备")));
    deviceCombo_ = new QComboBox();
    if (manager_ != nullptr) {
        for (const QString& id : manager_->ids()) {
            const service::DeviceSpec spec = manager_->spec(id);
            deviceCombo_->addItem(spec.name, id);
        }
    }
    controls->addWidget(deviceCombo_);

    // 通道多选（借鉴 PI Vision/Grafana）：勾选多个通道即可叠加对比，最多 4 条。
    controls->addWidget(new QLabel(QStringLiteral("通道")));
    channelList_ = new QListWidget();
    channelList_->setMaximumHeight(78);
    channelList_->setMinimumWidth(120);
    channelList_->setToolTip(QStringLiteral(
        "勾选要对比的通道（最多同时显示 4 条曲线）；勾选变化立即刷新。"));
    controls->addWidget(channelList_);

    controls->addWidget(new QLabel(QStringLiteral("时间范围")));
    rangeCombo_ = new QComboBox();
    rangeCombo_->addItem(QStringLiteral("最近 5 分钟"), 5);
    rangeCombo_->addItem(QStringLiteral("最近 15 分钟"), 15);
    rangeCombo_->addItem(QStringLiteral("最近 60 分钟"), 60);
    rangeCombo_->addItem(QStringLiteral("最近 4 小时"), 240);
    rangeCombo_->addItem(QStringLiteral("最近 8 小时"), 480);
    rangeCombo_->addItem(QStringLiteral("最近 24 小时"), 1440);
    rangeCombo_->setToolTip(QStringLiteral(
        "查询窗口越长，图表按抽样显示；数据来自秒级摘要表 sample_summary。"));
    controls->addWidget(rangeCombo_);

    // 快捷时间范围（借鉴 Grafana/PI Vision）：一键切换常用窗口。
    auto* quickGroup = new QWidget();
    auto* quickLayout = new QHBoxLayout(quickGroup);
    quickLayout->setContentsMargins(0, 0, 0, 0);
    quickLayout->setSpacing(4);
    const QVector<QPair<QString, int>> quickRanges = {
        {QStringLiteral("5m"), 5},
        {QStringLiteral("1h"), 60},
        {QStringLiteral("8h"), 480},
        {QStringLiteral("24h"), 1440},
    };
    for (const auto& quick : quickRanges) {
        auto* button = new QPushButton(quick.first);
        button->setProperty("role", QStringLiteral("chip"));
        button->setToolTip(QStringLiteral("切换到最近 %1").arg(quick.first));
        quickLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, minutes = quick.second] {
            const int index = rangeCombo_->findData(minutes);
            if (index >= 0) {
                rangeCombo_->setCurrentIndex(index);
                refresh();
            }
        });
    }
    controls->addWidget(quickGroup);

    auto* refreshButton = new QPushButton(QStringLiteral("刷新"));
    controls->addWidget(refreshButton);
    controls->addStretch(1);

    infoLabel_ = new QLabel(QStringLiteral("--"));
    controls->addWidget(infoLabel_);

    root->addLayout(controls);

    view_ = new TrendView();
    root->addWidget(view_, 1);

    connect(refreshButton, &QPushButton::clicked, this, &TrendPage::refresh);
    connect(deviceCombo_, &QComboBox::currentIndexChanged, this, [this] {
        populateChannels(deviceCombo_->currentData().toString());
        refresh();
    });
    connect(channelList_, &QListWidget::itemChanged, this, &TrendPage::refresh);
    connect(rangeCombo_, &QComboBox::currentIndexChanged, this, &TrendPage::refresh);

    populateChannels(deviceCombo_->currentData().toString());
}

// 按设备通道数重建可勾选列表；默认勾选 CH0。
void TrendPage::populateChannels(const QString& deviceId)
{
    if (channelList_ == nullptr)
        return;

    int count = static_cast<int>(protocol::kDefaultChannelCount);
    if (manager_ != nullptr) {
        const int configured = manager_->config(deviceId).channelCount;
        if (configured > 0)
            count = configured;
    }

    QSignalBlocker blocker(channelList_);
    channelList_->clear();
    for (int channel = 0; channel < count; ++channel) {
        auto* item = new QListWidgetItem(QStringLiteral("CH%1").arg(channel), channelList_);
        item->setData(Qt::UserRole, channel);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(channel == 0 ? Qt::Checked : Qt::Unchecked);
    }
}

void TrendPage::refresh()
{
    if (repository_ == nullptr || view_ == nullptr)
        return;

    const QString deviceId = deviceCombo_->currentData().toString();
    if (deviceId.isEmpty()) {
        view_->clearData();
        infoLabel_->setText(QStringLiteral("没有可用设备"));
        return;
    }
    if (channelList_ == nullptr || channelList_->count() == 0)
        populateChannels(deviceId);

    const int minutes = rangeCombo_->currentData().toInt();
    const qint64 toSec = QDateTime::currentSecsSinceEpoch();
    const qint64 fromSec = toSec - static_cast<qint64>(minutes) * 60;

    // 勾选的通道各查一条曲线；最多 4 条，避免线条过多看不清。
    QVector<TrendSeries> series;
    QStringList labels;
    int points = 0;
    for (int i = 0; i < channelList_->count(); ++i) {
        QListWidgetItem* item = channelList_->item(i);
        if (item->checkState() != Qt::Checked)
            continue;

        const int channel = item->data(Qt::UserRole).toInt();
        TrendSeries one;
        one.channel = channel;
        one.rows = repository_->summaryTrend(deviceId, channel, fromSec, toSec, 5000);
        points += one.rows.size();
        labels.append(QStringLiteral("CH%1").arg(channel));
        series.append(one);
        if (series.size() >= 4)
            break;
    }

    if (series.isEmpty()) {
        view_->clearData();
        infoLabel_->setText(QStringLiteral("请勾选至少一个通道"));
        return;
    }

    view_->setSeries(series, fromSec, toSec);
    infoLabel_->setText(QStringLiteral("%1 个采样秒 · %2")
                            .arg(points)
                            .arg(labels.join(QStringLiteral(", "))));
}

} // namespace aegis::ui
