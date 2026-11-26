#include "ui/label_report.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QMarginsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>

namespace aegis::ui {

namespace {

// 标签上的文字按页高比例排版，换尺寸不用改代码。
void drawText(QPainter& painter, const QRectF& rect, const QString& text,
              const QFont& font, const QColor& color = Qt::black,
              int flags = Qt::AlignLeft | Qt::AlignVCenter)
{
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(rect, flags, text);
}

} // namespace

bool exportPassLabel(const QString& path, const core::StationConfig& station,
                     const QString& sn, const QString& product,
                     const QString& orderNo, const QString& operatorName,
                     const QString& shift, qint64 finishedAt,
                     QString* errorMessage)
{
    if (path.trimmed().isEmpty() || sn.trimmed().isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("标签路径或 SN 为空");
        return false;
    }

    QFile probe(path);
    if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("无法写入标签文件：%1").arg(probe.errorString());
        return false;
    }
    probe.close();

    QPdfWriter writer(path);
    const QPageSize pageSize(QSizeF(station.labelWidthMm, station.labelHeightMm),
                             QPageSize::Millimeter, QStringLiteral("AegisLabel"),
                             QPageSize::ExactMatch);
    writer.setPageSize(pageSize);
    writer.setPageMargins(QMarginsF(2, 2, 2, 2), QPageLayout::Millimeter);
    writer.setResolution(600);
    writer.setTitle(QStringLiteral("合格证 %1").arg(sn));

    QPainter painter(&writer);
    if (!painter.isActive()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("标签绘制初始化失败");
        return false;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(QRectF(0, 0, writer.width(), writer.height()), Qt::white);

    const double width = writer.width();
    const double height = writer.height();
    const double margin = width * 0.05;

    auto row = [&](double top, double rowHeight) {
        return QRectF(margin, height * top, width - margin * 2.0, height * rowHeight);
    };

    QFont small;
    small.setPointSizeF(5.5);
    QFont normal;
    normal.setPointSizeF(7.0);
    QFont snFont;
    snFont.setPointSizeF(13.0);
    snFont.setBold(true);
    QFont verdictFont;
    verdictFont.setPointSizeF(11.0);
    verdictFont.setBold(true);
    QFont titleFont;
    titleFont.setPointSizeF(8.0);
    titleFont.setBold(true);

    drawText(painter, row(0.02, 0.10),
             QStringLiteral("%1　合格证").arg(station.reportCompany), titleFont,
             QColor(0x2c, 0x3e, 0x50));
    drawText(painter, row(0.14, 0.24), sn, snFont, Qt::black);

    drawText(painter, row(0.40, 0.11),
             QStringLiteral("产品：%1").arg(product.isEmpty() ? QStringLiteral("—")
                                                             : product),
             normal);
    drawText(painter, row(0.52, 0.11),
             QStringLiteral("工单：%1").arg(orderNo.isEmpty() ? QStringLiteral("—")
                                                             : orderNo),
             normal);

    const QString time = QDateTime::fromMSecsSinceEpoch(
                             finishedAt > 0 ? finishedAt
                                            : QDateTime::currentMSecsSinceEpoch())
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    drawText(painter, row(0.64, 0.10), time, small, QColor(0x7f, 0x8c, 0x8d));

    const QString operatorText =
        shift.isEmpty() ? operatorName : QStringLiteral("%1 / %2").arg(operatorName, shift);
    drawText(painter, row(0.74, 0.10),
             QStringLiteral("检验：%1").arg(operatorText.isEmpty() ? QStringLiteral("—")
                                                                  : operatorText),
             small, QColor(0x7f, 0x8c, 0x8d));

    drawText(painter, row(0.84, 0.14), QStringLiteral("PASS"), verdictFont,
             QColor(0x27, 0xae, 0x60), Qt::AlignRight | Qt::AlignVCenter);

    painter.end();

    if (!QFileInfo::exists(path) || QFileInfo(path).size() <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("标签写入失败（文件为空）");
        return false;
    }
    return true;
}

} // namespace aegis::ui
