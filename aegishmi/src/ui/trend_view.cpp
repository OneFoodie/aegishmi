#include "ui/trend_view.h"

#include <QDateTime>
#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPolygonF>

#include <algorithm>
#include <cmath>

namespace aegis::ui {

namespace {
constexpr int kLeftMargin = 70;
constexpr int kTopMargin = 34; // 顶部留出图例空间
constexpr int kRightMargin = 16;
constexpr int kBottomMargin = 30;

// 通道配色：沿用 ISA-101 的低饱和基调，多曲线仍可区分。
QColor seriesColor(int channel)
{
    static const QColor palette[] = {
        QColor(QStringLiteral("#37474f")), QColor(QStringLiteral("#00695c")),
        QColor(QStringLiteral("#4e6e8e")), QColor(QStringLiteral("#8d6e63")),
        QColor(QStringLiteral("#6a5a8c")), QColor(QStringLiteral("#827717")),
        QColor(QStringLiteral("#546e7a")), QColor(QStringLiteral("#ad1457")),
    };
    return palette[static_cast<std::size_t>(std::abs(channel)) % 8];
}
} // namespace

TrendView::TrendView(QWidget* parent)
    : QWidget(parent)
{
    setMinimumSize(640, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void TrendView::setSeries(const QVector<TrendSeries>& series, qint64 fromSec, qint64 toSec)
{
    series_ = series;
    fromSec_ = fromSec;
    toSec_ = std::max(fromSec + 1, toSec);
    update();
}

void TrendView::setData(const QVector<storage::SummaryRow>& rows, qint64 fromSec, qint64 toSec)
{
    TrendSeries single;
    single.channel = 0;
    single.rows = rows;
    setSeries({single}, fromSec, toSec);
}

void TrendView::clearData()
{
    series_.clear();
    fromSec_ = 0;
    toSec_ = 0;
    update();
}

void TrendView::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRectF full = rect();
    painter.fillRect(full, QColor(0xFA, 0xFA, 0xFA));

    const QRectF plot(kLeftMargin, kTopMargin,
                      full.width() - kLeftMargin - kRightMargin,
                      full.height() - kTopMargin - kBottomMargin);
    if (plot.width() <= 1 || plot.height() <= 1)
        return;
    painter.fillRect(plot, QColor(0xFF, 0xFF, 0xFF));

    // ---- 网格 ----
    QPen gridPen(QColor(0xE2, 0xE2, 0xE2));
    gridPen.setWidth(1);
    painter.setPen(gridPen);
    constexpr int kVerticalDivisions = 5;
    constexpr int kHorizontalDivisions = 5;
    for (int i = 0; i <= kHorizontalDivisions; ++i) {
        const double y = plot.top() + plot.height() * i / kHorizontalDivisions;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    for (int i = 0; i <= kVerticalDivisions; ++i) {
        const double x = plot.left() + plot.width() * i / kVerticalDivisions;
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    QFontMetrics fm(painter.font());
    painter.setPen(QColor(0x60, 0x60, 0x60));

    // 横轴时间刻度
    for (int i = 0; i <= kVerticalDivisions; ++i) {
        const double ratio = static_cast<double>(i) / kVerticalDivisions;
        const double x = plot.left() + plot.width() * ratio;
        const qint64 sec = fromSec_ + static_cast<qint64>((toSec_ - fromSec_) * ratio);
        const QString text =
            QDateTime::fromSecsSinceEpoch(sec).toString(QStringLiteral("HH:mm:ss"));
        painter.drawText(QRectF(x - 40, plot.bottom() + 4, 80, kBottomMargin - 4),
                         Qt::AlignHCenter | Qt::AlignTop, text);
    }

    // ---- 图例（多通道时显示通道号与颜色）----
    if (!series_.isEmpty()) {
        double legendX = plot.left();
        const double legendY = 10.0;
        for (const TrendSeries& item : series_) {
            const QColor color = seriesColor(item.channel);
            painter.fillRect(QRectF(legendX, legendY + 3, 10, 10), color);
            const QString label = QStringLiteral("CH%1").arg(item.channel);
            const int textWidth = fm.horizontalAdvance(label);
            painter.setPen(QColor(0x45, 0x4f, 0x55));
            painter.drawText(QRectF(legendX + 14, legendY - 2, textWidth + 4, 18),
                             Qt::AlignLeft | Qt::AlignVCenter, label);
            legendX += 14 + textWidth + 18;
        }
    }

    // ---- 计算总体数据范围 ----
    bool hasData = false;
    double valueMin = 0.0;
    double valueMax = 0.0;
    for (const TrendSeries& item : series_) {
        for (const storage::SummaryRow& row : item.rows) {
            if (!hasData) {
                valueMin = row.minValue;
                valueMax = row.maxValue;
                hasData = true;
            } else {
                valueMin = std::min(valueMin, row.minValue);
                valueMax = std::max(valueMax, row.maxValue);
            }
        }
    }

    if (!hasData) {
        painter.setPen(QColor(0x90, 0x90, 0x90));
        painter.drawText(plot, Qt::AlignCenter,
                         QStringLiteral("该时间段暂无摘要数据（先在监控页连接设备采集）"));
        return;
    }

    if (valueMax - valueMin < 1.0)
        valueMax = valueMin + 1.0;
    const double padding = (valueMax - valueMin) * 0.08;
    valueMin -= padding;
    valueMax += padding;

    // 纵轴数值刻度
    for (int i = 0; i <= kHorizontalDivisions; ++i) {
        const double ratio = static_cast<double>(i) / kHorizontalDivisions;
        const double y = plot.top() + plot.height() * ratio;
        const double value = valueMax - (valueMax - valueMin) * ratio;
        painter.drawText(QRectF(0, y - fm.height() / 2.0, kLeftMargin - 6, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(value, 'f', 0));
    }

    auto mapX = [&](qint64 sec) {
        return plot.left()
            + (static_cast<double>(sec - fromSec_) / static_cast<double>(toSec_ - fromSec_))
                  * plot.width();
    };
    auto mapY = [&](double value) {
        return plot.top()
            + (1.0 - (value - valueMin) / (valueMax - valueMin)) * plot.height();
    };

    painter.save();
    painter.setClipRect(plot);

    // 单通道时绘制 min/max 包络；多通道只画 avg，避免互相遮挡。
    if (series_.size() == 1) {
        const QVector<storage::SummaryRow>& rows = series_.first().rows;
        if (rows.size() >= 2) {
            QPolygonF band;
            band.reserve(rows.size() * 2);
            for (const auto& row : rows)
                band.append(QPointF(mapX(row.epochSec), mapY(row.minValue)));
            for (int i = rows.size() - 1; i >= 0; --i) {
                const auto& row = rows.at(i);
                band.append(QPointF(mapX(row.epochSec), mapY(row.maxValue)));
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0x4E, 0x6E, 0x8E, 50));
            painter.drawPolygon(band);
            painter.setBrush(Qt::NoBrush);
        }
    }

    for (const TrendSeries& item : series_) {
        if (item.rows.isEmpty())
            continue;

        const QColor color = seriesColor(item.channel);
        if (item.rows.size() >= 2) {
            QPolygonF avg;
            avg.reserve(item.rows.size());
            for (const auto& row : item.rows)
                avg.append(QPointF(mapX(row.epochSec), mapY(row.avgValue)));
            QPen linePen(color);
            linePen.setWidth(1);
            linePen.setCosmetic(true);
            painter.setPen(linePen);
            painter.drawPolyline(avg);
        } else {
            painter.setPen(QPen(color, 2));
            painter.drawPoint(
                QPointF(mapX(item.rows.first().epochSec), mapY(item.rows.first().avgValue)));
        }
    }

    painter.restore();
}

} // namespace aegis::ui
