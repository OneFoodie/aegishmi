#include "ui/shift_page.h"

#include "security/auth_service.h"
#include "storage/test_repository.h"

#include <QAbstractItemView>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

QTableWidgetItem* makeItem(const QString& text,
                           Qt::Alignment alignment = Qt::AlignCenter)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

} // namespace

ShiftPage::ShiftPage(storage::TestRepository* repository,
                     security::AuthService* auth,
                     const core::StationConfig& station, QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
    , auth_(auth)
    , station_(station)
{
    auto* root = new QVBoxLayout(this);

    shiftLabel_ = new QLabel(QStringLiteral("班次：—"));
    shiftLabel_->setStyleSheet(QStringLiteral("font-size:14pt;font-weight:bold;color:#2c3e50;"));
    root->addWidget(shiftLabel_);

    qualificationLabel_ = new QLabel(QStringLiteral("资质：未启用（所有操作员均可测试）"));
    qualificationLabel_->setWordWrap(true);
    qualificationLabel_->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    root->addWidget(qualificationLabel_);

    qualificationTable_ = new QTableWidget(0, 3);
    qualificationTable_->setHorizontalHeaderLabels(
        {QStringLiteral("产品"), QStringLiteral("授权操作员"), QStringLiteral("说明")});
    qualificationTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    qualificationTable_->verticalHeader()->setVisible(false);
    qualificationTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    qualificationTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    qualificationTable_->setMaximumHeight(150);
    root->addWidget(qualificationTable_);

    auto* handoverRow = new QHBoxLayout();
    handoverRow->addWidget(new QLabel(QStringLiteral("交接班")));
    handoverButton_ = new QPushButton(QStringLiteral("记录交接班…"));
    handoverRow->addWidget(handoverButton_);
    handoverRow->addStretch(1);
    root->addLayout(handoverRow);

    historyTable_ = new QTableWidget(0, 8);
    historyTable_->setHorizontalHeaderLabels(
        {QStringLiteral("时间"), QStringLiteral("班次"), QStringLiteral("工单"),
         QStringLiteral("交接人"), QStringLiteral("产量"), QStringLiteral("PASS"),
         QStringLiteral("FAIL"), QStringLiteral("备注")});
    historyTable_->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Stretch);
    historyTable_->verticalHeader()->setVisible(false);
    historyTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    historyTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    historyTable_->setAlternatingRowColors(true);
    root->addWidget(historyTable_, 1);

    connect(handoverButton_, &QPushButton::clicked, this, &ShiftPage::handover);

    refresh();
}

void ShiftPage::setActiveOrder(const QString& orderNo, const QString& product)
{
    orderNo_ = orderNo;
    product_ = product;
    refresh();
}

void ShiftPage::setOperationAllowed(bool allowed)
{
    operationAllowed_ = allowed;
    applyControlState();
}

void ShiftPage::applyControlState()
{
    handoverButton_->setEnabled(operationAllowed_);
    handoverButton_->setToolTip(operationAllowed_
                                    ? QString()
                                    : QStringLiteral("观察员仅可查看，不能记录交接班"));
}

void ShiftPage::refresh()
{
    const QDateTime now = QDateTime::currentDateTime();
    const QString shift = station_.shiftAt(now);
    const QString user = auth_ != nullptr ? auth_->currentUser().name
                                          : QStringLiteral("—");

    shiftLabel_->setText(
        QStringLiteral("当前时间：%1　班次：%2　操作员：%3　工单：%4")
            .arg(now.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                 shift.isEmpty() ? QStringLiteral("未配置") : shift, user,
                 orderNo_.isEmpty() ? QStringLiteral("—") : orderNo_));

    // ---- 资质表：只展示与当前产品相关的规则，避免配置一大屏 ----
    qualificationTable_->setRowCount(0);
    if (!station_.qualificationEnabled) {
        qualificationLabel_->setText(
            QStringLiteral("资质：未启用（configs/station.json 的 qualification.enabled）"));
    } else {
        QStringList products = station_.qualifiedOperators.keys();
        products.sort();

        int rows = 0;
        for (const QString& key : products) {
            // 有具体产品时隐藏 "*" 默认规则之外的不相关产品，保持表格精简。
            if (!product_.isEmpty() && key != product_ && key != QLatin1String("*"))
                continue;
            qualificationTable_->insertRow(rows);
            const QString display = key == QLatin1String("*")
                                        ? QStringLiteral("默认（*）")
                                        : key;
            qualificationTable_->setItem(rows, 0, makeItem(display));
            const QStringList users = station_.qualifiedOperators.value(key);
            qualificationTable_->setItem(
                rows, 1,
                makeItem(users.isEmpty() ? QStringLiteral("不限")
                                         : users.join(QStringLiteral("、")),
                         Qt::AlignLeft | Qt::AlignVCenter));
            const bool qualified = station_.isOperatorQualified(product_, user);
            qualificationTable_->setItem(
                rows, 2,
                makeItem(qualified ? QStringLiteral("当前用户可用")
                                   : QStringLiteral("当前用户无该产品资质")));
            ++rows;
        }

        qualificationLabel_->setText(
            QStringLiteral("资质：已启用　当前产品：%1　当前用户：%2（%3）")
                .arg(product_.isEmpty() ? QStringLiteral("—") : product_, user,
                     station_.isOperatorQualified(product_, user)
                         ? QStringLiteral("已授权")
                         : QStringLiteral("未授权，无法开始测试")));
    }

    // ---- 交接班历史 ----
    historyTable_->setRowCount(0);
    if (repository_ != nullptr && repository_->isOpen()) {
        const QVector<storage::HandoverRecord> records = repository_->recentHandovers(100);
        historyTable_->setRowCount(records.size());
        for (int row = 0; row < records.size(); ++row) {
            const storage::HandoverRecord& record = records.at(row);
            historyTable_->setItem(
                row, 0,
                makeItem(QDateTime::fromMSecsSinceEpoch(record.createdAt)
                             .toString(QStringLiteral("MM-dd HH:mm:ss"))));
            historyTable_->setItem(row, 1, makeItem(record.shift));
            historyTable_->setItem(row, 2, makeItem(record.orderNo));
            historyTable_->setItem(row, 3, makeItem(record.operatorName));
            historyTable_->setItem(row, 4, makeItem(QString::number(record.units)));
            historyTable_->setItem(row, 5, makeItem(QString::number(record.passRuns)));
            historyTable_->setItem(row, 6, makeItem(QString::number(record.failRuns)));
            historyTable_->setItem(row, 7,
                                   makeItem(record.note, Qt::AlignLeft | Qt::AlignVCenter));
        }
    }

    applyControlState();
}

void ShiftPage::handover()
{
    if (!operationAllowed_ || repository_ == nullptr || !repository_->isOpen())
        return;

    const QString user = auth_ != nullptr ? auth_->currentUser().name
                                          : QStringLiteral("—");
    const QString shift = station_.shiftAt(QDateTime::currentDateTime());
    const storage::WorkOrderStats stats =
        orderNo_.isEmpty() ? storage::WorkOrderStats{}
                           : repository_->statsOfOrder(orderNo_);

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("记录交接班"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* info = new QLabel(
        QStringLiteral("工单：%1　产品：%2\n班次：%3　交接人：%4\n"
                       "当前工单累计：产量 %5，PASS %6，FAIL %7，FPY %8%")
            .arg(orderNo_.isEmpty() ? QStringLiteral("—") : orderNo_,
                 product_.isEmpty() ? QStringLiteral("—") : product_,
                 shift.isEmpty() ? QStringLiteral("未配置") : shift, user)
            .arg(stats.units)
            .arg(stats.passRuns)
            .arg(stats.failRuns)
            .arg(stats.fpy * 100.0, 0, 'f', 1));
    info->setWordWrap(true);
    layout->addWidget(info);

    layout->addWidget(new QLabel(QStringLiteral("交接备注（未结事项、设备状态、异常等）")));
    auto* noteEdit = new QPlainTextEdit();
    noteEdit->setPlaceholderText(QStringLiteral("例如：治具接触偶发不良，已通知工艺；待返修 2 件"));
    noteEdit->setMaximumHeight(140);
    layout->addWidget(noteEdit);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认交接"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted)
        return;

    storage::HandoverRecord record;
    record.orderNo = orderNo_;
    record.lineId = station_.lineId;
    record.stationId = station_.stationId;
    record.shift = shift;
    record.operatorName = user;
    record.units = stats.units;
    record.passRuns = stats.passRuns;
    record.failRuns = stats.failRuns;
    record.note = noteEdit->toPlainText().trimmed();
    record.createdAt = QDateTime::currentMSecsSinceEpoch();

    QString error;
    if (!repository_->saveHandover(record, &error)) {
        QMessageBox::warning(this, QStringLiteral("交接班"), error);
        return;
    }

    if (auth_ != nullptr) {
        auth_->audit(QStringLiteral("交接班"), orderNo_, QStringLiteral("成功"),
                     QStringLiteral("班次=%1 产量=%2 PASS=%3 FAIL=%4 备注=%5")
                         .arg(record.shift)
                         .arg(record.units)
                         .arg(record.passRuns)
                         .arg(record.failRuns)
                         .arg(record.note));
    }
    refresh();
}

} // namespace aegis::ui
