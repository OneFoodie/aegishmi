#include "ui/dashboard_page.h"

#include "security/auth_service.h"
#include "storage/test_repository.h"
#include "ui/approval_dialog.h"
#include "ui/work_order_dialog.h"

#include <QAbstractItemView>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

QTableWidgetItem* makeItem(const QString& text, Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QString uploadText(const QString& status)
{
    if (status == QLatin1String("SENT"))
        return QStringLiteral("已上传");
    if (status == QLatin1String("PENDING"))
        return QStringLiteral("待上传");
    if (status == QLatin1String("DEAD"))
        return QStringLiteral("失败");
    return QStringLiteral("未上传");
}

QColor verdictColor(const QString& verdict)
{
    if (verdict == QLatin1String("PASS"))
        return QColor(0x27, 0xAE, 0x60);
    if (verdict == QLatin1String("FAIL"))
        return QColor(0xC0, 0x39, 0x2B);
    return QColor(0xE6, 0x7E, 0x22);
}

} // namespace

DashboardPage::DashboardPage(storage::TestRepository* repository,
                             security::AuthService* auth,
                             const core::StationConfig& station,
                             QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
    , station_(station)
{
    auto* root = new QVBoxLayout(this);

    stationLabel_ = new QLabel(QStringLiteral("工位：%1　线体：%2　治具：%3")
                                   .arg(station_.stationId,
                                        station_.lineId,
                                        station_.fixtureId.isEmpty()
                                            ? QStringLiteral("—")
                                            : station_.fixtureId));
    stationLabel_->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    root->addWidget(stationLabel_);

    fixtureLabel_ = new QLabel(QStringLiteral("治具：—"));
    fixtureLabel_->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    root->addWidget(fixtureLabel_);

    // ---- Andon 停线横幅 ----
    auto* andonRow = new QHBoxLayout();
    andonBanner_ = new QLabel();
    andonBanner_->setWordWrap(true);
    andonBanner_->setStyleSheet(QStringLiteral(
        "background:#fdecea;color:#c0392b;border:1px solid #f5b7b1;"
        "border-radius:4px;padding:6px;font-weight:bold;"));
    andonBanner_->hide();
    andonClearButton_ = new QPushButton(QStringLiteral("解除停线"));
    andonClearButton_->hide();
    andonRow->addWidget(andonBanner_, 1);
    andonRow->addWidget(andonClearButton_);
    root->addLayout(andonRow);
    connect(andonClearButton_, &QPushButton::clicked, this, &DashboardPage::clearAndon);

    // ---- 工单工具条 ----
    auto* orderRow = new QHBoxLayout();
    orderRow->addWidget(new QLabel(QStringLiteral("工单")));
    orderCombo_ = new QComboBox();
    orderCombo_->setMinimumWidth(320);
    orderRow->addWidget(orderCombo_, 1);

    newButton_ = new QPushButton(QStringLiteral("新建工单"));
    closeButton_ = new QPushButton(QStringLiteral("关闭工单"));
    refreshButton_ = new QPushButton(QStringLiteral("刷新"));
    mesRetryButton_ = new QPushButton(QStringLiteral("重传失败项"));
    mesRetryButton_->setEnabled(false);
    faiConfirmButton_ = new QPushButton(QStringLiteral("首件确认"));
    faiResetButton_ = new QPushButton(QStringLiteral("重新首件"));
    fixtureResetButton_ = new QPushButton(QStringLiteral("治具复位/校准"));
    snImportButton_ = new QPushButton(QStringLiteral("导入 SN 清单…"));
    snImportButton_->setToolTip(
        QStringLiteral("导入本工单发货清单（CSV/TXT，一行一个 SN），测试时校验 SN 是否在清单内"));
    reportButton_ = new QPushButton(QStringLiteral("导出日报"));
    reportButton_->setToolTip(QStringLiteral("按日期+班次导出运行数/良率 CSV"));
    faiConfirmButton_->setEnabled(false);
    faiResetButton_->setEnabled(false);
    fixtureResetButton_->setEnabled(false);
    snImportButton_->setEnabled(false);

    orderRow->addWidget(newButton_);
    orderRow->addWidget(closeButton_);
    orderRow->addWidget(faiConfirmButton_);
    orderRow->addWidget(faiResetButton_);
    orderRow->addWidget(fixtureResetButton_);
    orderRow->addWidget(snImportButton_);
    orderRow->addWidget(reportButton_);
    orderRow->addWidget(mesRetryButton_);
    orderRow->addWidget(refreshButton_);
    root->addLayout(orderRow);

    orderInfo_ = new QLabel(QStringLiteral("未选择工单"));
    orderInfo_->setWordWrap(true);
    root->addWidget(orderInfo_);

    auto* progressRow = new QHBoxLayout();
    progress_ = new QProgressBar();
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progressText_ = new QLabel(QStringLiteral("产量 —"));
    progressRow->addWidget(progress_, 1);
    progressRow->addWidget(progressText_);
    root->addLayout(progressRow);

    // ---- 指标卡片（借鉴 Grafana/ThingsBoard 的 KPI 卡：标题小、数值大、颜色表语义）----
    auto* metrics = new QHBoxLayout();
    metrics->setSpacing(8);
    auto makeMetric = [](const QString& title, QLabel** value,
                         const QString& tone) -> QWidget* {
        auto* box = new QFrame();
        box->setProperty("role", QStringLiteral("metric"));
        auto* layout = new QVBoxLayout(box);
        layout->setContentsMargins(12, 8, 12, 8);
        layout->setSpacing(2);

        auto* titleLabel = new QLabel(title);
        titleLabel->setProperty("role", QStringLiteral("metricTitle"));
        *value = new QLabel(QStringLiteral("--"));
        (*value)->setProperty("role", QStringLiteral("metricValue"));
        if (!tone.isEmpty())
            (*value)->setProperty("tone", tone);

        layout->addWidget(titleLabel);
        layout->addWidget(*value);
        return box;
    };

    metrics->addWidget(makeMetric(QStringLiteral("总测试"), &totalValue_, QString()));
    metrics->addWidget(makeMetric(QStringLiteral("PASS"), &passValue_, QStringLiteral("ok")));
    metrics->addWidget(makeMetric(QStringLiteral("FAIL"), &failValue_, QStringLiteral("error")));
    metrics->addWidget(makeMetric(QStringLiteral("直通率 FPY"), &fpyValue_, QStringLiteral("ok")));
    metrics->addWidget(makeMetric(QStringLiteral("UPH"), &uphValue_, QString()));
    metrics->addWidget(makeMetric(QStringLiteral("复测通过"), &retestPassValue_, QStringLiteral("ok")));
    metrics->addWidget(makeMetric(QStringLiteral("待返修"), &pendingRepairValue_, QStringLiteral("warn")));
    metrics->addWidget(makeMetric(QStringLiteral("MES"), &mesValue_, QStringLiteral("warn")));
    root->addLayout(metrics);

    // ---- 不良 Pareto：步骤 + 原因 ----
    auto* paretoRow = new QHBoxLayout();

    auto* stepBox = new QVBoxLayout();
    stepBox->addWidget(new QLabel(QStringLiteral("不良步骤（Pareto）")));
    paretoTable_ = new QTableWidget(0, 3);
    paretoTable_->setHorizontalHeaderLabels(
        {QStringLiteral("步骤"), QStringLiteral("动作"), QStringLiteral("失败次数")});
    paretoTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    paretoTable_->verticalHeader()->setVisible(false);
    paretoTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    paretoTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    paretoTable_->setMaximumHeight(160);
    stepBox->addWidget(paretoTable_);

    auto* reasonBox = new QVBoxLayout();
    reasonBox->addWidget(new QLabel(QStringLiteral("不良原因（Pareto）")));
    paretoReasonTable_ = new QTableWidget(0, 2);
    paretoReasonTable_->setHorizontalHeaderLabels(
        {QStringLiteral("原因"), QStringLiteral("失败次数")});
    paretoReasonTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    paretoReasonTable_->verticalHeader()->setVisible(false);
    paretoReasonTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    paretoReasonTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    paretoReasonTable_->setMaximumHeight(160);
    reasonBox->addWidget(paretoReasonTable_);

    paretoRow->addLayout(stepBox, 1);
    paretoRow->addLayout(reasonBox, 1);
    root->addLayout(paretoRow);

    // ---- 工步耗时 Top（瓶颈分析）----
    root->addWidget(new QLabel(QStringLiteral("工步耗时 Top（瓶颈）")));
    slowTable_ = new QTableWidget(0, 5);
    slowTable_->setHorizontalHeaderLabels({QStringLiteral("步骤"),
                                           QStringLiteral("动作"),
                                           QStringLiteral("执行次数"),
                                           QStringLiteral("平均耗时"),
                                           QStringLiteral("最长耗时")});
    slowTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    slowTable_->verticalHeader()->setVisible(false);
    slowTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    slowTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    slowTable_->setMaximumHeight(140);
    root->addWidget(slowTable_);

    // ---- 最近测试 ----
    root->addWidget(new QLabel(QStringLiteral("最近测试")));
    recentTable_ = new QTableWidget(0, 6);
    recentTable_->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                             QStringLiteral("SN"),
                                             QStringLiteral("方案"),
                                             QStringLiteral("判定"),
                                             QStringLiteral("复测"),
                                             QStringLiteral("MES")});
    recentTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    recentTable_->verticalHeader()->setVisible(false);
    recentTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recentTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    recentTable_->setAlternatingRowColors(true);
    root->addWidget(recentTable_, 1);

    connect(newButton_, &QPushButton::clicked, this, &DashboardPage::createOrder);
    connect(closeButton_, &QPushButton::clicked, this, &DashboardPage::closeOrder);
    connect(faiConfirmButton_, &QPushButton::clicked, this, &DashboardPage::confirmFai);
    connect(faiResetButton_, &QPushButton::clicked, this, &DashboardPage::resetFai);
    connect(fixtureResetButton_, &QPushButton::clicked, this, &DashboardPage::resetFixture);
    connect(snImportButton_, &QPushButton::clicked, this, &DashboardPage::importSnList);
    connect(reportButton_, &QPushButton::clicked, this, &DashboardPage::exportDailyReport);
    connect(refreshButton_, &QPushButton::clicked, this, &DashboardPage::refresh);
    connect(mesRetryButton_, &QPushButton::clicked, this,
            [this] { emit mesRetryRequested(); });
    connect(orderCombo_, &QComboBox::currentIndexChanged,
            this, &DashboardPage::onOrderChanged);

    refresh();
}

void DashboardPage::refresh()
{
    refreshOrderList();
    refreshStats();
}

void DashboardPage::refreshOrderList()
{
    if (repository_ == nullptr || !repository_->isOpen()) {
        orderCombo_->clear();
        activeOrderNo_.clear();
        activeProduct_.clear();
        updateOrderInfo();
        return;
    }

    const QString previous = activeOrderNo_;
    const QVector<storage::WorkOrder> orders = repository_->workOrders(200);

    updatingCombo_ = true;
    {
        QSignalBlocker blocker(orderCombo_);
        orderCombo_->clear();
        for (const storage::WorkOrder& order : orders) {
            if (order.status != QLatin1String("ACTIVE"))
                continue; // 已关闭工单不再用于新测试
            orderCombo_->addItem(QStringLiteral("%1　·　%2").arg(order.orderNo, order.product),
                                 order.orderNo);
        }

        int index = orderCombo_->findData(activeOrderNo_);
        if (index < 0 && orderCombo_->count() > 0)
            index = 0;

        if (index >= 0) {
            orderCombo_->setCurrentIndex(index);
            activeOrderNo_ = orderCombo_->itemData(index).toString();
            activeProduct_ = repository_->findWorkOrder(activeOrderNo_).product;
        } else {
            activeOrderNo_.clear();
            activeProduct_.clear();
        }
    }
    updatingCombo_ = false;

    updateOrderInfo();

    if (activeOrderNo_ != previous)
        emit activeOrderChanged(activeOrderNo_, activeProduct_);
}

void DashboardPage::updateOrderInfo()
{
    if (activeOrderNo_.isEmpty()) {
        orderInfo_->setStyleSheet(QStringLiteral("color:#c0392b"));
        orderInfo_->setText(QStringLiteral("未选择工单：请先新建/选择工单，再到「自动测试」页扫码测试"));
        progress_->setValue(0);
        progressText_->setText(QStringLiteral("产量 —"));
        return;
    }

    const storage::WorkOrder order = repository_->findWorkOrder(activeOrderNo_);
    const storage::WorkOrderStats stats = repository_->statsOfOrder(activeOrderNo_);

    QString faiText = QStringLiteral("不要求");
    if (order.faiRequired) {
        if (order.faiStatus == QLatin1String("PENDING"))
            faiText = QStringLiteral("待首件（%1 件）").arg(order.faiRequiredCount);
        else if (order.faiStatus == QLatin1String("WAITING"))
            faiText = QStringLiteral("首件待确认");
        else if (order.faiStatus == QLatin1String("REJECTED"))
            faiText = QStringLiteral("首件不合格");
        else
            faiText = QStringLiteral("已确认");
    }

    const int whitelistCount = repository_ != nullptr && repository_->isOpen()
                                   ? repository_->snWhitelistCount(activeOrderNo_)
                                   : 0;
    const QString whitelistText =
        whitelistCount > 0
            ? QStringLiteral("　SN清单：%1 条").arg(whitelistCount)
            : QString();

    orderInfo_->setStyleSheet(QStringLiteral("color:#2c3e50"));
    orderInfo_->setText(QStringLiteral("工单：%1　产品：%2　计划：%3　状态：%4　首件：%5%6")
                            .arg(order.orderNo,
                                 order.product,
                                 order.planQty > 0 ? QString::number(order.planQty)
                                                   : QStringLiteral("不限"),
                                 order.status,
                                 faiText,
                                 whitelistText));

    if (order.planQty > 0) {
        const int percent =
            qBound(0, static_cast<int>(stats.units * 100.0 / order.planQty), 100);
        progress_->setValue(percent);
        progressText_->setText(QStringLiteral("完成 %1 / %2（%3%）")
                                   .arg(stats.units)
                                   .arg(order.planQty)
                                   .arg(percent));
    } else {
        progress_->setValue(0);
        progressText_->setText(QStringLiteral("产量 %1（未设计划）").arg(stats.units));
    }
}

void DashboardPage::refreshStats()
{
    refreshAndon();

    // 没有工单：清空所有指标与表格。
    if (repository_ == nullptr || !repository_->isOpen() || activeOrderNo_.isEmpty()) {
        totalValue_->setText(QStringLiteral("--"));
        passValue_->setText(QStringLiteral("--"));
        failValue_->setText(QStringLiteral("--"));
        fpyValue_->setText(QStringLiteral("--"));
        uphValue_->setText(QStringLiteral("--"));
        retestPassValue_->setText(QStringLiteral("--"));
        pendingRepairValue_->setText(QStringLiteral("--"));
        mesValue_->setText(QStringLiteral("--"));
        deadCount_ = 0;
        canConfirmFai_ = false;
        canResetFai_ = false;
        paretoTable_->setRowCount(0);
        paretoReasonTable_->setRowCount(0);
        slowTable_->setRowCount(0);
        recentTable_->setRowCount(0);
        if (!station_.fixtureId.isEmpty())
            fixtureLabel_->setText(QStringLiteral("治具：%1（未开始使用）").arg(station_.fixtureId));
        applyControlState();
        return;
    }

    const storage::WorkOrder order = repository_->findWorkOrder(activeOrderNo_);
    const storage::WorkOrderStats stats = repository_->statsOfOrder(activeOrderNo_);

    totalValue_->setText(QString::number(stats.totalRuns));
    passValue_->setText(QString::number(stats.passRuns));
    failValue_->setText(QString::number(stats.failRuns));
    fpyValue_->setText(QStringLiteral("%1%").arg(stats.fpy * 100.0, 0, 'f', 1));
    uphValue_->setText(QStringLiteral("%1").arg(stats.uph, 0, 'f', 1));
    retestPassValue_->setText(QString::number(stats.retestPassRuns));
    pendingRepairValue_->setText(QString::number(stats.pendingRepairUnits));

    const int pending = repository_->uploadCountByStatus(QStringLiteral("PENDING"));
    const int dead = repository_->uploadCountByStatus(QStringLiteral("DEAD"));
    mesValue_->setText(QStringLiteral("待传 %1 / 失败 %2").arg(pending).arg(dead));
    deadCount_ = dead;

    // 首件按钮仅在对应状态可用。
    canConfirmFai_ = order.faiRequired
                  && order.faiStatus == QLatin1String("WAITING");
    canResetFai_ = order.faiRequired
                && order.faiStatus == QLatin1String("REJECTED");

    // ---- 治具状态 ----
    if (station_.fixtureId.isEmpty()) {
        fixtureLabel_->setText(QStringLiteral("治具：未配置"));
        fixtureLabel_->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    } else {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const storage::FixtureStatus fs = repository_->fixtureStatus(station_.fixtureId);
        const qint64 due = fs.calibratedUntil > 0
                               ? fs.calibratedUntil
                               : station_.defaultFixtureDueMs(nowMs);

        QString state = QStringLiteral("正常");
        QString color = QStringLiteral("#27ae60");
        if (due > 0 && nowMs > due) {
            state = QStringLiteral("校准已过期");
            color = QStringLiteral("#c0392b");
        } else if (station_.fixtureMaxRuns > 0 && fs.runCount >= station_.fixtureMaxRuns) {
            state = QStringLiteral("寿命已到");
            color = QStringLiteral("#c0392b");
        }

        const QString used = station_.fixtureMaxRuns > 0
                                 ? QStringLiteral("%1 / %2").arg(fs.runCount).arg(station_.fixtureMaxRuns)
                                 : QString::number(fs.runCount);
        fixtureLabel_->setText(
            QStringLiteral("治具：%1　已用 %2 次　校准至 %3　%4")
                .arg(station_.fixtureId,
                     used,
                     due > 0 ? QDateTime::fromMSecsSinceEpoch(due)
                                   .toString(QStringLiteral("yyyy-MM-dd"))
                             : QStringLiteral("未配置"),
                     state));
        fixtureLabel_->setStyleSheet(QStringLiteral("color:%1").arg(color));
    }

    applyControlState();
    updateOrderInfo();

    // ---- Pareto ----
    const QVector<storage::FailureRank> ranks =
        repository_->topFailureSteps(activeOrderNo_, 10);
    paretoTable_->setRowCount(ranks.size());
    for (int row = 0; row < ranks.size(); ++row) {
        paretoTable_->setItem(row, 0,
                              makeItem(ranks.at(row).stepName,
                                       Qt::AlignLeft | Qt::AlignVCenter));
        paretoTable_->setItem(row, 1, makeItem(ranks.at(row).action));
        paretoTable_->setItem(row, 2, makeItem(QString::number(ranks.at(row).count)));
    }

    // ---- 不良原因 Pareto ----
    const QVector<storage::ReasonRank> reasons =
        repository_->topFailureReasons(activeOrderNo_, 10);
    paretoReasonTable_->setRowCount(reasons.size());
    for (int row = 0; row < reasons.size(); ++row) {
        paretoReasonTable_->setItem(row, 0,
                                    makeItem(reasons.at(row).reason,
                                             Qt::AlignLeft | Qt::AlignVCenter));
        paretoReasonTable_->setItem(row, 1, makeItem(QString::number(reasons.at(row).count)));
    }

    // ---- 工步耗时 Top（瓶颈）----
    const QVector<storage::StepDurationRank> slow =
        repository_->topSlowSteps(activeOrderNo_, 8);
    slowTable_->setRowCount(slow.size());
    for (int row = 0; row < slow.size(); ++row) {
        const storage::StepDurationRank& rank = slow.at(row);
        slowTable_->setItem(row, 0,
                            makeItem(rank.stepName, Qt::AlignLeft | Qt::AlignVCenter));
        slowTable_->setItem(row, 1, makeItem(rank.action));
        slowTable_->setItem(row, 2, makeItem(QString::number(rank.runs)));
        slowTable_->setItem(row, 3,
                            makeItem(QStringLiteral("%1 ms").arg(rank.avgMs, 0, 'f', 0)));
        slowTable_->setItem(row, 4,
                            makeItem(QStringLiteral("%1 ms").arg(rank.maxMs)));
    }

    // ---- 最近测试 ----
    const QVector<storage::HistoryRun> runs =
        repository_->runsOfOrder(activeOrderNo_, 50);
    recentTable_->setRowCount(runs.size());
    for (int row = 0; row < runs.size(); ++row) {
        const storage::HistoryRun& run = runs.at(row);
        recentTable_->setItem(
            row, 0,
            makeItem(QDateTime::fromMSecsSinceEpoch(run.startedAt)
                         .toString(QStringLiteral("MM-dd HH:mm:ss"))));
        recentTable_->setItem(row, 1, makeItem(run.sn));
        recentTable_->setItem(row, 2,
                              makeItem(run.flowName, Qt::AlignLeft | Qt::AlignVCenter));

        auto* verdictItem = makeItem(run.verdict);
        verdictItem->setForeground(verdictColor(run.verdict));
        recentTable_->setItem(row, 3, verdictItem);

        recentTable_->setItem(row, 4,
                              makeItem(run.retest ? QStringLiteral("是")
                                                  : QStringLiteral("否")));
        recentTable_->setItem(row, 5, makeItem(uploadText(run.uploadStatus)));
    }
}

void DashboardPage::onOrderChanged(int index)
{
    if (updatingCombo_ || index < 0 || repository_ == nullptr)
        return;

    const QString orderNo = orderCombo_->itemData(index).toString();
    if (orderNo.isEmpty() || orderNo == activeOrderNo_)
        return;

    const storage::WorkOrder order = repository_->findWorkOrder(orderNo);
    if (order.status != QLatin1String("ACTIVE"))
        return;

    activeOrderNo_ = orderNo;
    activeProduct_ = order.product;
    updateOrderInfo();
    refreshStats();
    emit activeOrderChanged(activeOrderNo_, activeProduct_);
}

void DashboardPage::createOrder()
{
    if (repository_ == nullptr || !repository_->isOpen())
        return;

    WorkOrderDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QString error;
    if (!repository_->createWorkOrder(dialog.orderNo(), dialog.product(),
                                      dialog.planQty(), station_.faiEnabled,
                                      station_.faiSampleCount, dialog.flowPath(),
                                      &error)) {
        QMessageBox::warning(this, QStringLiteral("新建工单"), error);
        return;
    }

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("新建工单"), dialog.orderNo(),
                     QStringLiteral("成功"),
                     QStringLiteral("产品=%1 方案=%2")
                         .arg(dialog.product(),
                              dialog.flowPath().isEmpty() ? QStringLiteral("未绑定")
                                                          : dialog.flowPath()));
    }

    activeOrderNo_ = dialog.orderNo();
    activeProduct_ = dialog.product();
    refreshOrderList();
    refreshStats();
    emit activeOrderChanged(activeOrderNo_, activeProduct_);
}

void DashboardPage::closeOrder()
{
    if (repository_ == nullptr || activeOrderNo_.isEmpty())
        return;

    if (QMessageBox::question(this, QStringLiteral("关闭工单"),
                              QStringLiteral("确定关闭工单 %1？关闭后不再用于新测试。")
                                  .arg(activeOrderNo_))
        != QMessageBox::Yes) {
        return;
    }

    const QString closed = activeOrderNo_;
    QString error;
    if (!repository_->closeWorkOrder(closed, &error)) {
        QMessageBox::warning(this, QStringLiteral("关闭工单"), error);
        return;
    }

    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("关闭工单"), closed, QStringLiteral("成功"));

    activeOrderNo_.clear();
    activeProduct_.clear();
    refreshOrderList();
    refreshStats();

    // 没有下一个工单时也要通知测试页清空当前工单。
    if (activeOrderNo_.isEmpty())
        emit activeOrderChanged(QString(), QString());
}

void DashboardPage::setOrderSelectionEnabled(bool enabled)
{
    selectionEnabled_ = enabled;
    applyControlState();
}

void DashboardPage::setOperationAllowed(bool allowed)
{
    operationAllowed_ = allowed;
    applyControlState();
}

// 停线横幅：仅在启用 Andon 且存在未解除事件时显示。
void DashboardPage::refreshAndon()
{
    andonLocked_ = false;

    // 顺带把班次显示在工位信息里（班次会随时间变化，不能只在构造时算一次）。
    const QString shift = station_.shiftAt(QDateTime::currentDateTime());
    stationLabel_->setText(
        QStringLiteral("工位：%1　线体：%2　治具：%3　班次：%4")
            .arg(station_.stationId, station_.lineId,
                 station_.fixtureId.isEmpty() ? QStringLiteral("—") : station_.fixtureId,
                 shift.isEmpty() ? QStringLiteral("未配置") : shift));

    if (station_.andonEnabled && repository_ != nullptr && repository_->isOpen()) {
        const storage::AndonState state = repository_->andonState(station_.stationId);
        andonLocked_ = state.locked;
        if (state.locked) {
            andonBanner_->setText(
                QStringLiteral("⚠ Andon 停线：%1　%2　（%3 起，测试已锁定）")
                    .arg(state.reason,
                         state.detail.isEmpty() ? QStringLiteral("—")
                                                : state.detail,
                         QDateTime::fromMSecsSinceEpoch(state.createdAt)
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
            andonBanner_->show();
            andonClearButton_->show();
            applyControlState();
            return;
        }
    }

    andonBanner_->hide();
    andonClearButton_->hide();
    applyControlState();
}

void DashboardPage::clearAndon()
{
    if (repository_ == nullptr || !andonLocked_)
        return;

    // 停线解除是管理动作：只有管理员能放行。
    if (auth_ == nullptr || !auth_->can(security::Permission::Administer)) {
        QMessageBox::warning(this, QStringLiteral("解除停线"),
                             QStringLiteral("只有管理员可以解除 Andon 停线"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("解除停线"),
                              QStringLiteral("确认原因已排除并解除工位停线？"))
        != QMessageBox::Yes) {
        return;
    }

    // 双人复核：停线放行影响整线，管理员操作也需要第二人授权。
    QString approver;
    if (!requireSecondApproval(QStringLiteral("解除Andon停线"), &approver))
        return;

    const QString user = auth_->currentUser().name;
    if (!repository_->clearAndon(station_.stationId, user)) {
        QMessageBox::warning(this, QStringLiteral("解除停线"), QStringLiteral("写入失败"));
        return;
    }
    auth_->audit(QStringLiteral("解除Andon停线"), station_.stationId,
                 QStringLiteral("成功"),
                 QStringLiteral("解除人=%1 复核人=%2").arg(user, approver));
    refresh();
}

void DashboardPage::applyControlState()
{
    const bool canEdit = operationAllowed_ && selectionEnabled_;
    orderCombo_->setEnabled(canEdit);
    newButton_->setEnabled(canEdit);
    closeButton_->setEnabled(canEdit);
    // 重传不依赖"运行中"状态，只受角色限制。
    mesRetryButton_->setEnabled(operationAllowed_ && deadCount_ > 0);
    faiConfirmButton_->setEnabled(operationAllowed_ && canConfirmFai_);
    faiResetButton_->setEnabled(operationAllowed_ && canResetFai_);
    fixtureResetButton_->setEnabled(operationAllowed_ && !station_.fixtureId.isEmpty());
    snImportButton_->setEnabled(canEdit && !activeOrderNo_.isEmpty());
    reportButton_->setEnabled(operationAllowed_);
    // 解除停线只对管理员开放；观察员/操作员只能看到横幅。
    andonClearButton_->setEnabled(andonLocked_ && operationAllowed_ && auth_ != nullptr
                                  && auth_->can(security::Permission::Administer));
}

void DashboardPage::confirmFai()
{
    if (repository_ == nullptr || activeOrderNo_.isEmpty() || !operationAllowed_)
        return;
    if (QMessageBox::question(this, QStringLiteral("首件确认"),
                              QStringLiteral("确认首件检验合格并放行生产？"))
        != QMessageBox::Yes) {
        return;
    }

    const QString user = auth_ != nullptr ? auth_->currentUser().name
                                          : QStringLiteral("—");
    QString error;
    if (!repository_->confirmFai(activeOrderNo_, user, &error)) {
        QMessageBox::warning(this, QStringLiteral("首件确认"), error);
        return;
    }
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("首件确认"), activeOrderNo_,
                     QStringLiteral("成功"), QStringLiteral("确认人=%1").arg(user));
    refresh();
}

void DashboardPage::resetFai()
{
    if (repository_ == nullptr || activeOrderNo_.isEmpty() || !operationAllowed_)
        return;
    if (QMessageBox::question(this, QStringLiteral("重新首件"),
                              QStringLiteral("重新开始首件检验？已测试计数将清零。"))
        != QMessageBox::Yes) {
        return;
    }

    QString approver;
    if (!requireSecondApproval(QStringLiteral("重新首件"), &approver))
        return;

    QString error;
    if (!repository_->resetFai(activeOrderNo_, &error)) {
        QMessageBox::warning(this, QStringLiteral("重新首件"), error);
        return;
    }
    if (auth_ != nullptr)
        auth_->audit(QStringLiteral("重新首件"), activeOrderNo_, QStringLiteral("成功"),
                     QStringLiteral("复核人=%1").arg(approver));
    refresh();
}

void DashboardPage::resetFixture()
{
    if (repository_ == nullptr || station_.fixtureId.isEmpty() || !operationAllowed_)
        return;
    if (QMessageBox::question(
            this, QStringLiteral("治具复位/校准"),
            QStringLiteral("确认已完成治具保养/校准？使用次数将清零，校准有效期顺延 %1 天。")
                .arg(station_.fixtureCalibrationDays))
        != QMessageBox::Yes) {
        return;
    }

    QString approver;
    if (!requireSecondApproval(QStringLiteral("治具校准"), &approver))
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 dueMs = nowMs
        + static_cast<qint64>(qMax(1, station_.fixtureCalibrationDays)) * 24 * 3600 * 1000;
    QString error;
    if (!repository_->resetFixture(station_.fixtureId, dueMs, nowMs)) {
        QMessageBox::warning(this, QStringLiteral("治具复位"), QStringLiteral("写入失败"));
        return;
    }
    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("治具校准"), station_.fixtureId,
                     QStringLiteral("成功"),
                     QStringLiteral("下次校准=%1 复核人=%2")
                         .arg(QDateTime::fromMSecsSinceEpoch(dueMs)
                                  .toString(QStringLiteral("yyyy-MM-dd")),
                              approver));
    }
    refresh();
}

// 关键操作双人复核：取消或校验失败都留痕，返回 true 时 approverName 为复核人。
bool DashboardPage::requireSecondApproval(const QString& action,
                                          QString* approverName)
{
    if (auth_ == nullptr)
        return false;

    ApprovalDialog dialog(auth_, action, this);
    if (dialog.exec() != QDialog::Accepted) {
        auth_->audit(action, station_.stationId, QStringLiteral("已取消"),
                     QStringLiteral("双人复核未通过"));
        return false;
    }
    if (approverName != nullptr)
        *approverName = dialog.approverName();
    return true;
}

void DashboardPage::setActiveOrder(const QString& orderNo, bool notify){
    if (orderNo.isEmpty()) {
        activeOrderNo_.clear();
        activeProduct_.clear();
        updateOrderInfo();
        refreshStats();
        if (notify)
            emit activeOrderChanged(QString(), QString());
        return;
    }

    const int index = orderCombo_->findData(orderNo);
    if (index < 0)
        return; // 已关闭或不存在

    {
        QSignalBlocker blocker(orderCombo_);
        orderCombo_->setCurrentIndex(index);
    }
    activeOrderNo_ = orderNo;
    activeProduct_ = repository_ != nullptr ? repository_->findWorkOrder(orderNo).product
                                            : QString();
    updateOrderInfo();
    refreshStats();
    if (notify)
        emit activeOrderChanged(activeOrderNo_, activeProduct_);
}

// 导入发货清单：一行一个 SN（也兼容 CSV 取第一列）。
void DashboardPage::importSnList()
{
    if (repository_ == nullptr || !repository_->isOpen() || activeOrderNo_.isEmpty())
        return;

    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("导入 SN 清单"), QString(),
        QStringLiteral("清单文件 (*.csv *.txt);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("导入 SN 清单"),
                             QStringLiteral("无法读取文件：%1").arg(file.errorString()));
        return;
    }

    QStringList sns;
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine()).trimmed();
        line.remove(QChar(0xFEFF)); // 去掉 UTF-8 BOM
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        sns << line.split(QLatin1Char(',')).first().trimmed();
    }
    file.close();

    if (sns.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("导入 SN 清单"),
                             QStringLiteral("文件里没有可用的 SN"));
        return;
    }

    QString error;
    const int added = repository_->importSnWhitelist(activeOrderNo_, sns, &error);
    if (added < 0) {
        QMessageBox::warning(this, QStringLiteral("导入 SN 清单"), error);
        return;
    }

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("导入SN清单"), activeOrderNo_,
                     QStringLiteral("成功"),
                     QStringLiteral("文件=%1 新增=%2 总数=%3")
                         .arg(path)
                         .arg(added)
                         .arg(repository_->snWhitelistCount(activeOrderNo_)));
    }
    refresh();
    QMessageBox::information(
        this, QStringLiteral("导入 SN 清单"),
        QStringLiteral("新增 %1 条；当前工单清单共 %2 条。\n"
                       "清单非空时，测试只接受清单内的 SN。")
            .arg(added)
            .arg(repository_->snWhitelistCount(activeOrderNo_)));
}

// 日报：按日期 + 班次导出运行数/良率（默认今天，当前工单；无工单时导出全部）。
void DashboardPage::exportDailyReport()
{
    if (repository_ == nullptr || !repository_->isOpen() || !operationAllowed_)
        return;

    const QDateTime now = QDateTime::currentDateTime();
    const qint64 fromMs = QDateTime(now.date(), QTime(0, 0)).toMSecsSinceEpoch();
    const qint64 toMs = now.toMSecsSinceEpoch();

    const QVector<storage::ProductionReportRow> rows =
        repository_->productionReport(fromMs, toMs, activeOrderNo_);
    if (rows.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("导出日报"),
                                 QStringLiteral("今天暂无运行记录"));
        return;
    }

    QString base = QStringLiteral("daily_%1%2.csv")
                       .arg(activeOrderNo_.isEmpty() ? QStringLiteral("all")
                                                     : activeOrderNo_,
                            now.toString(QStringLiteral("_yyyyMMdd")));
    base.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")),
                 QStringLiteral("_"));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出日报"), base, QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("导出日报"),
                             QStringLiteral("无法写入文件：%1").arg(file.errorString()));
        return;
    }
    file.write("\xEF\xBB\xBF");
    file.write(QStringLiteral("日期,班次,运行数,PASS,FAIL,产量,PASS数,良率%\r\n").toUtf8());
    for (const storage::ProductionReportRow& row : rows) {
        const QString line =
            QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8\r\n")
                .arg(row.date, row.shift)
                .arg(row.runs)
                .arg(row.passRuns)
                .arg(row.failRuns)
                .arg(row.units)
                .arg(row.passUnits)
                .arg(row.yieldRate * 100.0, 0, 'f', 2);
        file.write(line.toUtf8());
    }
    file.close();

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("导出日报"), path, QStringLiteral("成功"),
                     QStringLiteral("工单=%1 行数=%2")
                         .arg(activeOrderNo_.isEmpty() ? QStringLiteral("全部")
                                                       : activeOrderNo_)
                         .arg(rows.size()));
    }
    QMessageBox::information(this, QStringLiteral("导出日报"),
                             QStringLiteral("已导出 %1 行：%2").arg(rows.size()).arg(path));
}

} // namespace aegis::ui
