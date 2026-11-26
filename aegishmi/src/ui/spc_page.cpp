#include "ui/spc_page.h"

#include "quality/spc.h"
#include "storage/test_repository.h"

#include <QComboBox>
#include <QAbstractItemView>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <QVariantList>

#include <algorithm>

namespace aegis::ui {

// =============================================================================
// ControlChart —— 单值控制图（I 图）
//
// 只画实测值序列 + 中心线 + 控制限 + 规格限；超出控制限的点标红。
// 数据量不大（≤500 点），全量绘制即可。
// =============================================================================
class ControlChart : public QWidget {
public:
    explicit ControlChart(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(260);
    }

    void setResult(const quality::SpcResult& result, const QString& title,
                   const QString& unit)
    {
        result_ = result;
        title_ = title;
        unit_ = unit;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), Qt::white);

        const QRectF plot(70, 28, width() - 92, height() - 70);
        painter.setPen(QColor(0x2c, 0x3e, 0x50));
        painter.drawText(QPointF(12, 18), title_.isEmpty() ? QStringLiteral("控制图")
                                                           : title_);

        if (result_.n < 2) {
            painter.setPen(QColor(0x7f, 0x8c, 0x8d));
            painter.drawText(plot, Qt::AlignCenter,
                             QStringLiteral("样本不足（至少 2 个数据点才能绘制控制图）"));
            return;
        }

        // ---- 纵向范围：数据、控制限、规格限都纳入 ----
        double low = *std::min_element(result_.values.begin(), result_.values.end());
        double high = *std::max_element(result_.values.begin(), result_.values.end());
        low = std::min(low, result_.lclX);
        high = std::max(high, result_.uclX);
        if (result_.hasUsl)
            high = std::max(high, result_.usl);
        if (result_.hasLsl)
            low = std::min(low, result_.lsl);
        if (high - low < 1e-9) {
            high += 1.0;
            low -= 1.0;
        }
        const double margin = (high - low) * 0.08;
        low -= margin;
        high += margin;

        auto yFor = [&](double value) {
            return plot.bottom() - (value - low) / (high - low) * plot.height();
        };
        auto xFor = [&](int index) {
            if (result_.n == 1)
                return plot.center().x();
            return plot.left()
                + plot.width() * index / static_cast<double>(result_.n - 1);
        };

        // ---- 网格与刻度 ----
        painter.setPen(QPen(QColor(0xe8, 0xec, 0xef), 1));
        for (int i = 0; i <= 4; ++i) {
            const double y = plot.top() + plot.height() * i / 4.0;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            const double value = high - (high - low) * i / 4.0;
            painter.setPen(QColor(0x7f, 0x8c, 0x8d));
            painter.drawText(QRectF(0, y - 8, 62, 16), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(value, 'f', 2));
            painter.setPen(QPen(QColor(0xe8, 0xec, 0xef), 1));
        }
        painter.setPen(QColor(0x95, 0xa5, 0xa6));
        painter.drawLine(plot.bottomLeft(), plot.bottomRight());
        painter.drawText(QRectF(plot.left(), plot.bottom() + 6, plot.width(), 20),
                         Qt::AlignCenter,
                         QStringLiteral("按时间顺序（共 %1 个数据点）").arg(result_.n));

        // ---- 参考线：规格限（红虚线）、控制限（蓝虚线）、中心线（绿实线）----
        auto drawLine = [&](double value, const QColor& color, Qt::PenStyle style,
                            const QString& label) {
            painter.setPen(QPen(color, 1, style));
            const double y = yFor(value);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.drawText(QPointF(plot.right() + 4, y + 4), label);
        };

        if (result_.hasUsl)
            drawLine(result_.usl, QColor(0xc0, 0x39, 0x2b), Qt::DashLine,
                     QStringLiteral("USL"));
        if (result_.hasLsl)
            drawLine(result_.lsl, QColor(0xc0, 0x39, 0x2b), Qt::DashLine,
                     QStringLiteral("LSL"));
        drawLine(result_.uclX, QColor(0x29, 0x80, 0xb9), Qt::DashLine,
                 QStringLiteral("UCL"));
        drawLine(result_.lclX, QColor(0x29, 0x80, 0xb9), Qt::DashLine,
                 QStringLiteral("LCL"));
        drawLine(result_.mean, QColor(0x27, 0xae, 0x60), Qt::SolidLine,
                 QStringLiteral("CL"));

        // ---- 数据折线与点 ----
        painter.setPen(QPen(QColor(0x34, 0x49, 0x5e), 1.6));
        QPainterPath path;
        for (int i = 0; i < result_.values.size(); ++i) {
            const QPointF point(xFor(i), yFor(result_.values.at(i)));
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        painter.drawPath(path);

        for (int i = 0; i < result_.values.size(); ++i) {
            const QPointF point(xFor(i), yFor(result_.values.at(i)));
            if (result_.outOfControl.contains(i)) {
                painter.setBrush(QColor(0xc0, 0x39, 0x2b));
                painter.setPen(QPen(QColor(0xc0, 0x39, 0x2b), 1));
                painter.drawEllipse(point, 3.5, 3.5);
            } else {
                painter.setBrush(Qt::white);
                painter.setPen(QPen(QColor(0x34, 0x49, 0x5e), 1.2));
                painter.drawEllipse(point, 2.2, 2.2);
            }
        }
    }

private:
    quality::SpcResult result_;
    QString title_;
    QString unit_;
};

// =============================================================================
// SpcPage
// =============================================================================
SpcPage::SpcPage(storage::TestRepository* repository, QWidget* parent)
    : QWidget(parent)
    , repository_(repository)
{
    auto* root = new QVBoxLayout(this);

    auto* toolbar = new QHBoxLayout();
    toolbar->addWidget(new QLabel(QStringLiteral("工单")));
    orderCombo_ = new QComboBox();
    orderCombo_->setMinimumWidth(280);
    toolbar->addWidget(orderCombo_);

    toolbar->addWidget(new QLabel(QStringLiteral("测试项")));
    itemCombo_ = new QComboBox();
    itemCombo_->setMinimumWidth(320);
    toolbar->addWidget(itemCombo_, 1);

    refreshButton_ = new QPushButton(QStringLiteral("刷新"));
    toolbar->addWidget(refreshButton_);
    root->addLayout(toolbar);

    specLabel_ = new QLabel(QStringLiteral("规格限：—"));
    specLabel_->setStyleSheet(QStringLiteral("color:#7f8c8d"));
    root->addWidget(specLabel_);

    metrics_ = new QLabel(QStringLiteral("—"));
    metrics_->setTextFormat(Qt::RichText);
    root->addWidget(metrics_);

    chart_ = new ControlChart();
    root->addWidget(chart_, 2);

    auto* bottom = new QHBoxLayout();

    auto* valuesBox = new QVBoxLayout();
    valuesBox->addWidget(new QLabel(QStringLiteral("最近数据点（新→旧）")));
    valuesTable_ = new QTableWidget(0, 3);
    valuesTable_->setHorizontalHeaderLabels(
        {QStringLiteral("时间"), QStringLiteral("SN"), QStringLiteral("实测值")});
    valuesTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    valuesTable_->verticalHeader()->setVisible(false);
    valuesTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    valuesTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    valuesBox->addWidget(valuesTable_);

    auto* violationBox = new QVBoxLayout();
    violationBox->addWidget(new QLabel(QStringLiteral("判异命中")));
    violations_ = new QPlainTextEdit();
    violations_->setReadOnly(true);
    violationBox->addWidget(violations_);

    bottom->addLayout(valuesBox, 1);
    bottom->addLayout(violationBox, 1);
    root->addLayout(bottom, 1);

    connect(refreshButton_, &QPushButton::clicked, this, &SpcPage::refresh);
    connect(orderCombo_, &QComboBox::currentIndexChanged,
            this, &SpcPage::onOrderChanged);
    connect(itemCombo_, &QComboBox::currentIndexChanged,
            this, &SpcPage::onItemChanged);

    refresh();
}

void SpcPage::refresh()
{
    refreshOrders();
    refreshItems();
    updateChart();
}

void SpcPage::refreshOrders()
{
    const QString previous = orderCombo_->currentData().toString();
    orderCombo_->blockSignals(true);
    orderCombo_->clear();

    if (repository_ != nullptr && repository_->isOpen()) {
        // 分析历史数据，已关闭工单也保留在列表里。
        for (const storage::WorkOrder& order : repository_->workOrders(200)) {
            orderCombo_->addItem(
                QStringLiteral("%1　·　%2").arg(order.orderNo, order.product),
                order.orderNo);
        }
    }

    const int index = orderCombo_->findData(previous);
    if (index >= 0)
        orderCombo_->setCurrentIndex(index);
    orderCombo_->blockSignals(false);
}

void SpcPage::refreshItems()
{
    const QString orderNo = orderCombo_->currentData().toString();
    const QString previous = itemCombo_->currentData().toList().isEmpty()
        ? QString()
        : itemCombo_->currentData().toList().at(0).toString()
              + QLatin1Char('|')
              + itemCombo_->currentData().toList().at(1).toString();

    itemCombo_->blockSignals(true);
    itemCombo_->clear();

    if (repository_ != nullptr && repository_->isOpen() && !orderNo.isEmpty()) {
        for (const storage::SpcItem& item : repository_->spcItems(orderNo)) {
            const QString text =
                QStringLiteral("%1 / %2（%3 个样本）")
                    .arg(item.flowName.isEmpty() ? item.flowId : item.flowName,
                         item.stepName, QString::number(item.count));
            itemCombo_->addItem(text, QVariantList{item.flowId, item.stepId});

            if ((item.flowId + QLatin1Char('|') + item.stepId) == previous)
                itemCombo_->setCurrentIndex(itemCombo_->count() - 1);
        }
    }
    itemCombo_->blockSignals(false);
}

void SpcPage::onOrderChanged()
{
    refreshItems();
    updateChart();
}

void SpcPage::onItemChanged()
{
    updateChart();
}

void SpcPage::updateChart()
{
    const QString orderNo = orderCombo_->currentData().toString();
    const QVariantList key = itemCombo_->currentData().toList();

    if (repository_ == nullptr || !repository_->isOpen() || orderNo.isEmpty()
        || key.size() < 2) {
        specLabel_->setText(QStringLiteral("规格限：—"));
        metrics_->setText(QStringLiteral("暂无可用测试项（需要已有实测值且流程配置了上下限）"));
        chart_->setResult(quality::SpcResult{}, QStringLiteral("控制图"), QString());
        valuesTable_->setRowCount(0);
        violations_->setPlainText(QString());
        return;
    }

    const QString flowId = key.at(0).toString();
    const QString stepId = key.at(1).toString();

    // 找到当前项以取得规格限与名称。
    storage::SpcItem current;
    for (const storage::SpcItem& item : repository_->spcItems(orderNo)) {
        if (item.flowId == flowId && item.stepId == stepId) {
            current = item;
            break;
        }
    }

    const QVector<storage::SpcPoint> points =
        repository_->spcPoints(orderNo, flowId, stepId, 500);

    QVector<double> values;
    values.reserve(points.size());
    for (const storage::SpcPoint& point : points)
        values.append(point.value);

    const quality::SpcResult result = quality::compute(
        values, current.hasUsl, current.usl, current.hasLsl, current.lsl);

    specLabel_->setText(
        QStringLiteral("规格限：%1　单位：%2")
            .arg(current.hasUsl && current.hasLsl
                     ? QStringLiteral("%1 ~ %2")
                           .arg(current.lsl, 0, 'f', 2)
                           .arg(current.usl, 0, 'f', 2)
                     : current.hasUsl
                           ? QStringLiteral("≤ %1").arg(current.usl, 0, 'f', 2)
                           : QStringLiteral("≥ %1").arg(current.lsl, 0, 'f', 2),
                 current.unit.isEmpty() ? QStringLiteral("—") : current.unit));

    const QString grade = quality::gradeForCpk(result.cpk, result.hasCpk);
    const QString cpText = result.hasCp ? QString::number(result.cp, 'f', 2)
                                        : QStringLiteral("—");
    const QString cpkText = result.hasCpk ? QString::number(result.cpk, 'f', 2)
                                          : QStringLiteral("—");

    metrics_->setText(
        QStringLiteral(
            "<table cellspacing=\"0\" cellpadding=\"8\"><tr>"
            "<td style=\"background:#f4f6f8;\">样本 n<br><b style=\"font-size:16pt;\">%1</b></td>"
            "<td style=\"background:#f4f6f8;\">均值<br><b style=\"font-size:16pt;\">%2</b></td>"
            "<td style=\"background:#f4f6f8;\">σ<br><b style=\"font-size:16pt;\">%3</b></td>"
            "<td style=\"background:#f4f6f8;\">Cp<br><b style=\"font-size:16pt;\">%4</b></td>"
            "<td style=\"background:#f4f6f8;\">Cpk<br><b style=\"font-size:16pt;\">%5</b></td>"
            "<td style=\"background:#f4f6f8;\">等级<br><b style=\"font-size:16pt;\">%6</b></td>"
            "<td style=\"background:#f4f6f8;\">判异<br><b style=\"font-size:16pt;\">%7</b></td>"
            "</tr></table>")
            .arg(result.n)
            .arg(result.mean, 0, 'f', 2)
            .arg(result.sigma, 0, 'f', 3)
            .arg(cpText, cpkText, grade)
            .arg(result.violations.size()));

    chart_->setResult(result, QStringLiteral("%1 / %2").arg(flowId, current.stepName),
                      current.unit);

    // 最新 20 个数据点，按时间倒序展示。
    const int show = qMin(20, points.size());
    valuesTable_->setRowCount(show);
    for (int row = 0; row < show; ++row) {
        const storage::SpcPoint& point = points.at(points.size() - 1 - row);
        auto* timeItem = new QTableWidgetItem(
            QDateTime::fromMSecsSinceEpoch(point.at)
                .toString(QStringLiteral("MM-dd HH:mm:ss")));
        timeItem->setTextAlignment(Qt::AlignCenter);
        valuesTable_->setItem(row, 0, timeItem);

        auto* snItem = new QTableWidgetItem(point.sn);
        valuesTable_->setItem(row, 1, snItem);

        auto* valueItem = new QTableWidgetItem(QString::number(point.value, 'f', 2));
        valueItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        valuesTable_->setItem(row, 2, valueItem);
    }

    if (result.violations.isEmpty()) {
        violations_->setPlainText(QStringLiteral("未命中判异规则（受控）。"));
    } else {
        violations_->setPlainText(result.violations.join(QLatin1Char('\n')));
    }
}

} // namespace aegis::ui
