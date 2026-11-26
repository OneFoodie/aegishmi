#include "ui/alarm_page.h"

#include "service/alarm_monitor.h"
#include "ui/theme.h"

#include "security/auth_service.h"

#include <QColor>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

constexpr int kColumnCount = 9;

QTableWidgetItem* makeItem(const QString& text, Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString formatTime(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return QStringLiteral("—");
    return QDateTime::fromMSecsSinceEpoch(milliseconds)
        .toString(QStringLiteral("MM-dd HH:mm:ss"));
}

} // namespace

AlarmPage::AlarmPage(aegis::storage::SystemRepository* repository,
                     security::AuthService* auth,
                     service::AlarmMonitor* monitor,
                     QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
    , monitor_(monitor)
{
    auto* root = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    auto* refreshButton = new QPushButton(QStringLiteral("刷新"));
    ackButton_ = new QPushButton(QStringLiteral("确认选中"));
    ackAllButton_ = new QPushButton(QStringLiteral("全部确认"));
    shelveButton_ = new QPushButton(QStringLiteral("搁置 10 分钟"));
    ackButton_->setToolTip(QStringLiteral("把选中的报警标记为已确认（仍需处理实际问题）"));
    ackAllButton_->setToolTip(QStringLiteral("确认当前列表中所有未确认报警"));
    shelveButton_->setToolTip(QStringLiteral(
        "ISA-18.2 搁置：10 分钟内该通道不再产生新报警；已存在的报警仍会正常恢复"));
    info_ = new QLabel(QStringLiteral("暂无报警"));

    toolbar->addWidget(refreshButton);
    toolbar->addWidget(ackButton_);
    toolbar->addWidget(ackAllButton_);
    toolbar->addWidget(shelveButton_);
    toolbar->addWidget(info_, 1);

    table_ = new QTableWidget(0, kColumnCount);
    table_->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                       QStringLiteral("设备"),
                                       QStringLiteral("通道"),
                                       QStringLiteral("级别"),
                                       QStringLiteral("实测"),
                                       QStringLiteral("阈值"),
                                       QStringLiteral("状态"),
                                       QStringLiteral("确认人"),
                                       QStringLiteral("确认时间")});
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(true);

    root->addLayout(toolbar);
    root->addWidget(table_, 1);

    connect(refreshButton, &QPushButton::clicked, this, &AlarmPage::refresh);
    connect(ackButton_, &QPushButton::clicked, this, &AlarmPage::acknowledgeSelected);
    connect(ackAllButton_, &QPushButton::clicked, this, &AlarmPage::acknowledgeAll);
    connect(shelveButton_, &QPushButton::clicked, this, &AlarmPage::shelveSelected);

    refresh();
}

void AlarmPage::setAckAllowed(bool allowed)
{
    ackAllowed_ = allowed;
    ackButton_->setEnabled(allowed);
    ackAllButton_->setEnabled(allowed);
    shelveButton_->setEnabled(allowed && monitor_ != nullptr);
}

void AlarmPage::refresh()
{
    if (repository_ == nullptr || !repository_->isOpen()) {
        rows_.clear();
        table_->setRowCount(0);
        info_->setText(QStringLiteral("数据库未打开"));
        return;
    }

    rows_ = repository_->recentAlarms(500, false);
    table_->setRowCount(rows_.size());

    int active = 0;
    int unacknowledged = 0;
    for (int row = 0; row < rows_.size(); ++row) {
        const aegis::storage::AlarmRecord& alarm = rows_.at(row);
        if (alarm.clearedAtMs == 0)
            ++active;
        if (!alarm.acknowledged)
            ++unacknowledged;

        auto* timeItem = makeItem(formatTime(alarm.raisedAtMs));
        timeItem->setData(Qt::UserRole, alarm.id);

        table_->setItem(row, 0, timeItem);
        table_->setItem(row, 1, makeItem(alarm.deviceName.isEmpty() ? alarm.deviceId
                                                                    : alarm.deviceName,
                                         Qt::AlignLeft | Qt::AlignVCenter));
        table_->setItem(row, 2, makeItem(alarm.channelName));
        table_->setItem(row, 3, makeItem(alarm.level));
        table_->setItem(row, 4, makeItem(QStringLiteral("%1 %2")
                                             .arg(alarm.value, 0, 'f', 1)
                                             .arg(alarm.unit)));
        table_->setItem(row, 5, makeItem(QStringLiteral("%1")
                                             .arg(alarm.threshold, 0, 'f', 1)));

        auto* stateItem = makeItem(alarm.clearedAtMs == 0 ? QStringLiteral("报警中")
                                                          : QStringLiteral("已恢复"));
        stateItem->setForeground(alarm.clearedAtMs == 0 ? QColor(0xC0, 0x39, 0x2B)
                                                        : QColor(0x7F, 0x8C, 0x8D));
        table_->setItem(row, 6, stateItem);

        table_->setItem(row, 7, makeItem(alarm.acknowledged ? alarm.ackUser
                                                            : QStringLiteral("—")));
        table_->setItem(row, 8, makeItem(alarm.acknowledged
                                             ? formatTime(alarm.ackAtMs)
                                             : QStringLiteral("—")));
    }

    info_->setText(QStringLiteral("共 %1 条，未恢复 %2 条，未确认 %3 条")
                       .arg(rows_.size())
                       .arg(active)
                       .arg(unacknowledged));
}

int AlarmPage::selectedAlarmId() const
{
    const int row = selectedRow();
    if (row < 0 || row >= rows_.size())
        return -1;
    return rows_.at(row).id;
}

int AlarmPage::selectedRow() const
{
    const auto selected = table_->selectionModel() != nullptr
                              ? table_->selectionModel()->selectedRows()
                              : QModelIndexList();
    return selected.isEmpty() ? -1 : selected.first().row();
}

// ISA-18.2 搁置：临时抑制该通道的新报警，避免已知异常反复刷屏。
void AlarmPage::shelveSelected()
{
    if (!ackAllowed_ || monitor_ == nullptr)
        return;

    const int row = selectedRow();
    if (row < 0 || row >= rows_.size()) {
        info_->setText(QStringLiteral("请先选择一条报警"));
        return;
    }

    const aegis::storage::AlarmRecord& alarm = rows_.at(row);
    if (alarm.clearedAtMs != 0) {
        info_->setText(QStringLiteral("该报警已恢复，无需搁置"));
        return;
    }

    const QString name = alarm.channelName.isEmpty()
        ? QStringLiteral("CH%1").arg(alarm.channel)
        : alarm.channelName;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("搁置报警"),
        QStringLiteral("搁置 %1 %2 10 分钟？\n"
                       "搁置期间该通道不再产生新报警；已存在的报警仍会按条件恢复。")
            .arg(alarm.deviceName.isEmpty() ? alarm.deviceId : alarm.deviceName, name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes)
        return;

    monitor_->shelve(alarm.deviceId, alarm.channel, 10);
    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("搁置报警"),
                     QStringLiteral("%1/%2").arg(alarm.deviceId, name),
                     QStringLiteral("成功"), QStringLiteral("10 分钟"));
    }
    info_->setText(QStringLiteral("%1 已搁置 10 分钟（期间不产生新报警）").arg(name));
}

void AlarmPage::acknowledgeSelected()
{
    if (!ackAllowed_ || repository_ == nullptr)
        return;

    const int id = selectedAlarmId();
    if (id <= 0) {
        info_->setText(QStringLiteral("请先选择一条报警"));
        return;
    }

    const QString user = auth_ != nullptr ? auth_->currentUser().name
                                          : QStringLiteral("—");
    const bool ok = repository_->acknowledgeAlarm(id, user,
                                                  QDateTime::currentMSecsSinceEpoch());
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("确认报警"), QStringLiteral("#%1").arg(id),
                     ok ? QStringLiteral("成功") : QStringLiteral("失败"));
    refresh();
}

void AlarmPage::acknowledgeAll()
{
    if (!ackAllowed_ || repository_ == nullptr)
        return;

    const QString user = auth_ != nullptr ? auth_->currentUser().name
                                          : QStringLiteral("—");
    const int count = repository_->acknowledgeAll(user,
                                                  QDateTime::currentMSecsSinceEpoch());
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("确认报警"), QStringLiteral("全部"),
                     QStringLiteral("成功"), QStringLiteral("%1 条").arg(count));
    refresh();
}

} // namespace aegis::ui
