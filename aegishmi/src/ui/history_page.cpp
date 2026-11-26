#include "ui/history_page.h"

#include "storage/test_repository.h"
#include "ui/pdf_report.h"

#include "security/auth_service.h"

#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

QColor verdictColor(const QString& verdict)
{
    if (verdict == QLatin1String("PASS"))
        return QColor(0x27, 0xAE, 0x60);
    if (verdict == QLatin1String("FAIL"))
        return QColor(0xC0, 0x39, 0x2B);
    return QColor(0xE6, 0x7E, 0x22);
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

QTableWidgetItem* makeItem(const QString& text, Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

HistoryPage::HistoryPage(storage::TestRepository* repository,
                         security::AuthService* auth,
                         const core::StationConfig& station, QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
    , station_(station)
{
    buildUi();
    refresh();
}

void HistoryPage::setExportAllowed(bool allowed)
{
    exportAllowed_ = allowed;
    exportButton_->setEnabled(allowed && currentRunId_ > 0 && repository_ != nullptr);
    pdfButton_->setEnabled(allowed && currentRunId_ > 0 && repository_ != nullptr);
    batchPdfButton_->setEnabled(allowed && repository_ != nullptr
                                && runTable_->rowCount() > 0);
}

void HistoryPage::buildUi()
{
    auto* root = new QVBoxLayout(this);

    // ---- 工具栏 ----
    auto* toolbar = new QHBoxLayout();
    refreshButton_ = new QPushButton(QStringLiteral("刷新"));
    exportButton_ = new QPushButton(QStringLiteral("导出 CSV"));
    exportButton_->setEnabled(false);
    pdfButton_ = new QPushButton(QStringLiteral("导出 PDF"));
    pdfButton_->setEnabled(false);
    batchPdfButton_ = new QPushButton(QStringLiteral("批量 PDF…"));
    batchPdfButton_->setEnabled(false);
    info_ = new QLabel(QStringLiteral("暂无记录"));

    toolbar->addWidget(refreshButton_);
    toolbar->addWidget(exportButton_);
    toolbar->addWidget(pdfButton_);
    toolbar->addWidget(batchPdfButton_);
    toolbar->addWidget(info_, 1);

    // ---- 追溯检索：SN / 工单 / 操作员 / 方案 + 判定 ----
    auto* searchRow = new QHBoxLayout();
    searchEdit_ = new QLineEdit();
    searchEdit_->setPlaceholderText(
        QStringLiteral("追溯检索：输入 SN / 工单号 / 操作员 / 方案名（回车查询）"));
    verdictCombo_ = new QComboBox();
    verdictCombo_->addItem(QStringLiteral("全部判定"), QString());
    verdictCombo_->addItem(QStringLiteral("PASS"), QStringLiteral("PASS"));
    verdictCombo_->addItem(QStringLiteral("FAIL"), QStringLiteral("FAIL"));
    verdictCombo_->addItem(QStringLiteral("ABORTED"), QStringLiteral("ABORTED"));
    searchButton_ = new QPushButton(QStringLiteral("查询"));
    resetButton_ = new QPushButton(QStringLiteral("重置"));
    searchRow->addWidget(searchEdit_, 1);
    searchRow->addWidget(verdictCombo_);
    searchRow->addWidget(searchButton_);
    searchRow->addWidget(resetButton_);

    // ---- 运行列表 ----
    runTable_ = new QTableWidget(0, 9);
    runTable_->setHorizontalHeaderLabels({QStringLiteral("时间"),
                                          QStringLiteral("工单"),
                                          QStringLiteral("方案"),
                                          QStringLiteral("SN"),
                                          QStringLiteral("判定"),
                                          QStringLiteral("复测"),
                                          QStringLiteral("不良原因"),
                                          QStringLiteral("MES"),
                                          QStringLiteral("耗时")});
    runTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    runTable_->verticalHeader()->setVisible(false);
    runTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    runTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    runTable_->setSelectionMode(QAbstractItemView::SingleSelection);

    // ---- 步骤明细 ----
    stepTable_ = new QTableWidget(0, 7);
    stepTable_->setHorizontalHeaderLabels({QStringLiteral("#"),
                                           QStringLiteral("步骤"),
                                           QStringLiteral("动作"),
                                           QStringLiteral("状态"),
                                           QStringLiteral("实测值"),
                                           QStringLiteral("耗时"),
                                           QStringLiteral("说明")});
    stepTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    stepTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    stepTable_->verticalHeader()->setVisible(false);
    stepTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    stepTable_->setSelectionBehavior(QAbstractItemView::SelectRows);

    root->addLayout(toolbar);
    root->addLayout(searchRow);
    root->addWidget(runTable_, 1);
    root->addWidget(new QLabel(QStringLiteral("步骤明细")));
    root->addWidget(stepTable_, 1);

    connect(refreshButton_, &QPushButton::clicked, this, &HistoryPage::refresh);
    connect(searchButton_, &QPushButton::clicked, this, &HistoryPage::refresh);
    connect(searchEdit_, &QLineEdit::returnPressed, this, &HistoryPage::refresh);
    connect(resetButton_, &QPushButton::clicked, this, [this] {
        searchEdit_->clear();
        verdictCombo_->setCurrentIndex(0);
        refresh();
    });
    connect(exportButton_, &QPushButton::clicked, this, &HistoryPage::exportSelected);
    connect(pdfButton_, &QPushButton::clicked, this, &HistoryPage::exportPdf);
    connect(batchPdfButton_, &QPushButton::clicked, this, &HistoryPage::exportBatchPdf);
    connect(runTable_, &QTableWidget::itemSelectionChanged,
            this, &HistoryPage::onRunSelected);
}

void HistoryPage::refresh()
{
    currentRunId_ = -1;
    exportButton_->setEnabled(false);
    pdfButton_->setEnabled(false);
    batchPdfButton_->setEnabled(false);
    stepTable_->setRowCount(0);

    if (repository_ == nullptr || !repository_->isOpen()) {
        runTable_->setRowCount(0);
        info_->setText(QStringLiteral("数据库未打开"));
        return;
    }

    const QString keyword = searchEdit_->text().trimmed();
    const QString verdict = verdictCombo_->currentData().toString();
    const bool filtered = !keyword.isEmpty() || !verdict.isEmpty();

    const auto runs = filtered
                          ? repository_->searchRuns(keyword, verdict, 500)
                          : repository_->recentRuns();
    runTable_->setRowCount(runs.size());

    for (int row = 0; row < runs.size(); ++row) {
        const auto& run = runs.at(row);

        auto* timeItem = makeItem(
            QDateTime::fromMSecsSinceEpoch(run.startedAt).toString(QStringLiteral("MM-dd HH:mm:ss")));
        // 把数据库主键藏在第一列，选中时直接取，不需要额外映射。
        timeItem->setData(Qt::UserRole, run.id);

        runTable_->setItem(row, 0, timeItem);
        runTable_->setItem(row, 1, makeItem(run.workOrder));
        runTable_->setItem(row, 2, makeItem(run.flowName, Qt::AlignLeft | Qt::AlignVCenter));
        runTable_->setItem(row, 3, makeItem(run.sn, Qt::AlignLeft | Qt::AlignVCenter));

        auto* verdictItem = makeItem(run.verdict);
        verdictItem->setForeground(verdictColor(run.verdict));
        runTable_->setItem(row, 4, verdictItem);

        runTable_->setItem(row, 5, makeItem(run.retest ? QStringLiteral("是")
                                                       : QStringLiteral("否")));
        runTable_->setItem(row, 6, makeItem(run.failReason));
        runTable_->setItem(row, 7, makeItem(uploadText(run.uploadStatus)));
        runTable_->setItem(row, 8, makeItem(QStringLiteral("%1 ms").arg(run.durationMs)));
    }

    if (filtered) {
        info_->setText(QStringLiteral("筛选到 %1 条记录").arg(runs.size()));
    } else {
        info_->setText(runs.isEmpty() ? QStringLiteral("暂无记录")
                                      : QStringLiteral("共 %1 条记录").arg(runs.size()));
    }
    batchPdfButton_->setEnabled(exportAllowed_ && !runs.isEmpty()
                                && repository_ != nullptr);
}

void HistoryPage::onRunSelected()
{
    const auto selected = runTable_->selectionModel() != nullptr
                              ? runTable_->selectionModel()->selectedRows()
                              : QModelIndexList();
    if (selected.isEmpty()) {
        currentRunId_ = -1;
        exportButton_->setEnabled(false);
        return;
    }

    const int row = selected.first().row();
    auto* timeItem = runTable_->item(row, 0);
    if (timeItem == nullptr)
        return;

    currentRunId_ = timeItem->data(Qt::UserRole).toInt();
    exportButton_->setEnabled(exportAllowed_ && currentRunId_ > 0
                              && repository_ != nullptr);
    pdfButton_->setEnabled(exportAllowed_ && currentRunId_ > 0
                           && repository_ != nullptr);

    const auto steps = repository_->stepsOfRun(currentRunId_);
    stepTable_->setRowCount(steps.size());

    for (int i = 0; i < steps.size(); ++i) {
        const auto& step = steps.at(i);
        stepTable_->setItem(i, 0, makeItem(QString::number(step.index + 1)));
        stepTable_->setItem(i, 1, makeItem(step.name, Qt::AlignLeft | Qt::AlignVCenter));
        stepTable_->setItem(i, 2, makeItem(step.action));
        stepTable_->setItem(i, 3, makeItem(step.status));
        stepTable_->setItem(i, 4,
                            makeItem(step.hasMeasured
                                         ? QStringLiteral("%1 %2")
                                               .arg(step.measured, 0, 'f', 2)
                                               .arg(step.unit)
                                         : QStringLiteral("—")));
        stepTable_->setItem(i, 5, makeItem(QStringLiteral("%1 ms").arg(step.elapsedMs)));
        stepTable_->setItem(i, 6, makeItem(step.message, Qt::AlignLeft | Qt::AlignVCenter));
    }
}

void HistoryPage::exportSelected()
{
    if (repository_ == nullptr || currentRunId_ <= 0)
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出测试报告"),
        QStringLiteral("test_report_%1.csv").arg(currentRunId_),
        QStringLiteral("CSV 文件 (*.csv)"));
    if (path.isEmpty())
        return;

    QString error;
    if (repository_->exportCsv(currentRunId_, path, &error)) {
        info_->setText(QStringLiteral("已导出：%1").arg(path));
        if (auth_ != nullptr)
            auth_->audit(QStringLiteral("导出报表"), path, QStringLiteral("成功"),
                         QStringLiteral("运行 #%1").arg(currentRunId_));
    } else {
        info_->setText(QStringLiteral("导出失败：%1").arg(error));
        if (auth_ != nullptr)
            auth_->audit(QStringLiteral("导出报表"), path, QStringLiteral("失败"), error);
    }
}

void HistoryPage::exportPdf()
{
    if (repository_ == nullptr || currentRunId_ <= 0)
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 PDF 报告"),
        QStringLiteral("test_report_%1.pdf").arg(currentRunId_),
        QStringLiteral("PDF 文件 (*.pdf)"));
    if (path.isEmpty())
        return;

    QString error;
    if (exportRunPdf(repository_, currentRunId_, path, station_, &error)) {
        info_->setText(QStringLiteral("已导出 PDF：%1").arg(path));
        if (auth_ != nullptr) {
            auth_->audit(QStringLiteral("导出PDF"), path, QStringLiteral("成功"),
                         QStringLiteral("运行 #%1").arg(currentRunId_));
        }
    } else {
        info_->setText(QStringLiteral("导出失败：%1").arg(error));
        if (auth_ != nullptr)
            auth_->audit(QStringLiteral("导出PDF"), path, QStringLiteral("失败"), error);
    }
}

void HistoryPage::exportBatchPdf()
{
    if (repository_ == nullptr || runTable_->rowCount() == 0)
        return;

    // 批量导出范围为列表当前展示的记录（按时间倒序）。
    QVector<int> runIds;
    runIds.reserve(runTable_->rowCount());
    for (int row = 0; row < runTable_->rowCount(); ++row) {
        auto* item = runTable_->item(row, 0);
        if (item == nullptr)
            continue;
        const int runId = item->data(Qt::UserRole).toInt();
        if (runId > 0)
            runIds.append(runId);
    }
    if (runIds.isEmpty())
        return;

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("批量导出 PDF"),
        QStringLiteral("test_batch_%1.pdf")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))),
        QStringLiteral("PDF 文件 (*.pdf)"));
    if (path.isEmpty())
        return;

    QString error;
    if (exportRunsPdf(repository_, runIds, path, station_, &error)) {
        info_->setText(QStringLiteral("已批量导出 %1 份报告：%2")
                           .arg(runIds.size())
                           .arg(path));
        if (auth_ != nullptr) {
            auth_->audit(QStringLiteral("批量导出PDF"), path, QStringLiteral("成功"),
                         QStringLiteral("共 %1 份").arg(runIds.size()));
        }
    } else {
        info_->setText(QStringLiteral("批量导出失败：%1").arg(error));
        if (auth_ != nullptr)
            auth_->audit(QStringLiteral("批量导出PDF"), path, QStringLiteral("失败"), error);
    }
}

} // namespace aegis::ui
