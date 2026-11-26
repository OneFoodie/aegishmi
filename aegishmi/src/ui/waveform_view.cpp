#include "ui/waveform_view.h"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace aegis::ui {

namespace {

// 在已按像素桶聚合的点序列里二分查找最接近 time 的点值。
double valueNear(const std::deque<QPointF>& points, double time)
{
    if (points.empty())
        return std::numeric_limits<double>::quiet_NaN();

    int low = 0;
    int high = static_cast<int>(points.size()) - 1;
    while (low < high) {
        const int mid = (low + high) / 2;
        if (points[static_cast<std::size_t>(mid)].x() < time)
            low = mid + 1;
        else
            high = mid;
    }

    double best = points[static_cast<std::size_t>(low)].y();
    double bestDiff = std::abs(points[static_cast<std::size_t>(low)].x() - time);
    if (low > 0) {
        const double diff = std::abs(points[static_cast<std::size_t>(low - 1)].x() - time);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = points[static_cast<std::size_t>(low - 1)].y();
        }
    }
    return best;
}

// 前 8 个通道的固定配色；更多通道由 channelColor() 自动生成。
const QColor kPalette[8] = {
    QColor(0x2E, 0x86, 0xDE), QColor(0x27, 0xAE, 0x60), QColor(0xE6, 0x7E, 0x22),
    QColor(0x8E, 0x44, 0xAD), QColor(0xC0, 0x39, 0x2B), QColor(0x16, 0xA0, 0x85),
    QColor(0x8C, 0x6E, 0x0F), QColor(0x2C, 0x3E, 0x50),
};

constexpr int kLeftMargin = 64;   // 左侧留给纵轴刻度文字
constexpr int kTopMargin = 34;    // 顶部留给图例
constexpr int kRightMargin = 16;
constexpr int kBottomMargin = 28; // 底部留给横轴刻度文字
constexpr int kMaxLegendChannels = 8;

} // namespace

WaveformView::WaveformView(QWidget* parent)
    : QWidget(parent)
{
    // 让控件随窗口拉伸，并保证一个最小可读尺寸。
    setMinimumSize(640, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // 不透明绘制：跳过 Qt 默认的背景擦除，减少一次全控件填充。
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    // 鼠标游标需要实时跟踪位置。
    setMouseTracking(true);
}

void WaveformView::ensureChannels(int channelCount)
{
    if (channelCount == channelCount_)
        return;

    clearData();
    channelCount_ = std::max(0, channelCount);
    channels_.assign(static_cast<std::size_t>(channelCount_), {});
    bucketMin_.assign(static_cast<std::size_t>(channelCount_), 0.0);
    bucketMax_.assign(static_cast<std::size_t>(channelCount_), 0.0);
}

QColor WaveformView::channelColor(int channel) const
{
    if (channel < 0)
        return QColor(Qt::gray);
    if (channel < 8)
        return kPalette[channel];
    // 8 个以后用 HSV 生成，保证相邻通道颜色可区分。
    return QColor::fromHsv((channel * 47) % 360, 170, 190);
}

void WaveformView::appendChunk(double startSec, const std::int16_t* interleaved,
                               int pointCount, int channelCount, int sampleRateHz)
{
    // 防御性校验：时间戳/指针非法时直接丢弃（避免坐标被放大到天文数字）。
    if (interleaved == nullptr || pointCount <= 0 || channelCount <= 0)
        return;
    if (!std::isfinite(startSec) || std::abs(startSec) > 1.0e6)
        return;

    ensureChannels(channelCount);
    if (channelCount_ <= 0)
        return;

    // 一个像素对应一个时间桶：窗口秒数 / 绘图区像素宽度。
    const int plotWidth = std::max(100, width() - kLeftMargin - kRightMargin);
    bucketSec_ = std::max(1.0e-6, windowSec_ / plotWidth);
    const double rate = std::max(1, sampleRateHz);

    for (int i = 0; i < pointCount; ++i) {
        const double t = startSec + static_cast<double>(i) / rate;
        if (!std::isfinite(t) || std::abs(t) > 1.0e6)
            continue;

        const qint64 bucket = static_cast<qint64>(std::floor(t / bucketSec_));
        if (bucket != currentBucket_) {
            flushBucket(); // 上一个桶先落盘
            currentBucket_ = bucket;
            bucketMin_.assign(static_cast<std::size_t>(channelCount_),
                              std::numeric_limits<double>::infinity());
            bucketMax_.assign(static_cast<std::size_t>(channelCount_),
                              -std::numeric_limits<double>::infinity());
        }

        const std::int16_t* point = interleaved + static_cast<std::size_t>(i) * channelCount;
        for (int ch = 0; ch < channelCount_; ++ch) {
            const double value = static_cast<double>(point[ch]);
            bucketMin_[static_cast<std::size_t>(ch)] =
                std::min(bucketMin_[static_cast<std::size_t>(ch)], value);
            bucketMax_[static_cast<std::size_t>(ch)] =
                std::max(bucketMax_[static_cast<std::size_t>(ch)], value);
        }
    }

    lastTimeSec_ = std::max(lastTimeSec_,
                            startSec + static_cast<double>(pointCount - 1) / rate);
    hasData_ = true;
}

void WaveformView::flushBucket()
{
    if (currentBucket_ < 0 || channelCount_ <= 0)
        return;

    const double t0 = static_cast<double>(currentBucket_) * bucketSec_;
    const double t1 = t0 + bucketSec_;

    for (int ch = 0; ch < channelCount_; ++ch) {
        const std::size_t index = static_cast<std::size_t>(ch);
        if (!std::isfinite(bucketMin_[index]))
            continue; // 该通道在这个桶里没有数据

        // 每桶输出"谷、峰"两个点，保留毛刺又不增加多少绘制量。
        std::deque<QPointF>& points = channels_[index];
        points.emplace_back(t0, bucketMin_[index]);
        points.emplace_back(t1, bucketMax_[index]);
        while (static_cast<int>(points.size()) > kMaxPointsPerChannel)
            points.pop_front();
    }

    currentBucket_ = -1;
}

void WaveformView::setWindowSeconds(double seconds)
{
    // 1~10 秒：再小会看不清毛刺，再大超出内存里的点范围。
    const double clamped = std::clamp(seconds, 1.0, 10.0);
    if (std::abs(clamped - windowSec_) < 1e-6)
        return;
    // 先用旧桶宽收尾，避免半截桶被按新桶宽算出错误时间。
    flushBucket();
    windowSec_ = clamped;
    update();
}

void WaveformView::setChannelLimits(const QVector<ChannelLimit>& limits)
{
    limits_ = limits;
    update();
}

void WaveformView::wheelEvent(QWheelEvent* event)
{
    if (!hasData_) {
        QWidget::wheelEvent(event);
        return;
    }
    // 向上滚放大（窗口变短），向下滚缩小。
    const double factor = event->angleDelta().y() > 0 ? 1.0 / 1.25 : 1.25;
    setWindowSeconds(windowSec_ * factor);
    event->accept();
}

void WaveformView::mouseMoveEvent(QMouseEvent* event)
{
    if (lastPlot_.width() > 1) {
        cursorX_ = std::clamp(event->position().x(), lastPlot_.left(), lastPlot_.right());
        cursorActive_ = lastPlot_.adjusted(-1, -1, 1, 1).contains(event->position());
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void WaveformView::leaveEvent(QEvent* event)
{
    cursorActive_ = false;
    update();
    QWidget::leaveEvent(event);
}

// 左键定位 A 游标、右键定位 B 游标（右键不弹菜单）。
void WaveformView::mousePressEvent(QMouseEvent* event)
{
    if (!hasData_ || lastPlot_.width() <= 1) {
        QWidget::mousePressEvent(event);
        return;
    }

    const QPointF position = event->position();
    if (!lastPlot_.adjusted(-1, -1, 1, 1).contains(position)) {
        QWidget::mousePressEvent(event);
        return;
    }

    const double x = std::clamp(position.x(), lastPlot_.left(), lastPlot_.right());
    if (event->button() == Qt::LeftButton)
        cursorAX_ = x;
    else if (event->button() == Qt::RightButton)
        cursorBX_ = x;
    else {
        QWidget::mousePressEvent(event);
        return;
    }
    update();
    event->accept();
}

// 双击清除两个测量游标。
void WaveformView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (cursorAX_ >= 0.0 || cursorBX_ >= 0.0) {
        cursorAX_ = -1.0;
        cursorBX_ = -1.0;
        update();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

// 右键已用于定位 B 游标，这里吞掉上下文菜单。
void WaveformView::contextMenuEvent(QContextMenuEvent* event)
{
    event->accept();
}

void WaveformView::clearData()
{
    for (auto& channel : channels_)
        channel.clear();
    channels_.clear();
    bucketMin_.clear();
    bucketMax_.clear();
    channelCount_ = 0;
    currentBucket_ = -1;
    hasData_ = false;
    lastTimeSec_ = 0.0;
    // 像素坐标在清空后失去意义，双游标一并清除。
    cursorAX_ = -1.0;
    cursorBX_ = -1.0;
    cursorActive_ = false;
    update();
}

// =============================================================================
// paintEvent：一次完整重绘
//
// 坐标映射：
//   x 像素 = 绘图区左边 + (t - (最新时刻-窗口)) / 窗口 * 宽度
//   y 像素 = 绘图区上边 + (1 - (v - 最小值)/(最大值-最小值)) * 高度
// =============================================================================
void WaveformView::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    // 关闭抗锯齿：折线是高频刷新的热点，抗锯齿代价高而收益低。
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRectF full = rect();

    // 1) 背景
    painter.fillRect(full, QColor(0xFA, 0xFA, 0xFA));

    // 2) 计算绘图区（四周留出刻度文字空间）
    const QRectF plot(kLeftMargin,
                      kTopMargin,
                      full.width() - kLeftMargin - kRightMargin,
                      full.height() - kTopMargin - kBottomMargin);
    if (plot.width() <= 1 || plot.height() <= 1)
        return;

    painter.fillRect(plot, QColor(0xFF, 0xFF, 0xFF));

    // 3) 时间轴范围：始终显示"最新时刻往前 windowSec_ 秒"
    const double tEnd = hasData_ ? lastTimeSec_ : windowSec_;
    const double tStart = tEnd - windowSec_;
    // 游标换算需要知道本次绘图的映射关系。
    lastPlot_ = plot;
    lastTStart_ = tStart;

    // 4) 画网格与刻度
    QPen gridPen(QColor(0xE0, 0xE0, 0xE0));
    gridPen.setWidth(1);
    painter.setPen(gridPen);

    constexpr int kVerticalDivisions = 5;
    constexpr int kHorizontalDivisions = 5;

    for (int i = 0; i <= kHorizontalDivisions; ++i) {
        const double ratio = static_cast<double>(i) / kHorizontalDivisions;
        const double y = plot.top() + ratio * plot.height();
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    for (int i = 0; i <= kVerticalDivisions; ++i) {
        const double ratio = static_cast<double>(i) / kVerticalDivisions;
        const double x = plot.left() + ratio * plot.width();
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }

    // 5) 刻度文字
    painter.setPen(QColor(0x60, 0x60, 0x60));
    QFontMetrics fm(painter.font());

    for (int i = 0; i <= kVerticalDivisions; ++i) {
        const double ratio = static_cast<double>(i) / kVerticalDivisions;
        const double x = plot.left() + ratio * plot.width();
        const double t = tStart + ratio * windowSec_;
        painter.drawText(QRectF(x - 30, plot.bottom() + 4, 60, kBottomMargin - 4),
                         Qt::AlignHCenter | Qt::AlignTop, QString::number(t, 'f', 1));
    }

    for (int i = 0; i <= kHorizontalDivisions; ++i) {
        const double ratio = static_cast<double>(i) / kHorizontalDivisions;
        const double y = plot.top() + ratio * plot.height();
        const double value = kValueMax - ratio * (kValueMax - kValueMin);
        painter.drawText(QRectF(0, y - fm.height() / 2.0, kLeftMargin - 6, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(static_cast<int>(value)));
    }

    // 6) 图例：最多显示 8 个通道，其余用文字概括（避免顶部挤满）
    {
        const int legendCount = std::min(channelCount_, kMaxLegendChannels);
        int x = kLeftMargin;
        for (int ch = 0; ch < legendCount; ++ch) {
            QPen pen(channelColor(ch));
            pen.setWidth(2);
            painter.setPen(pen);
            painter.drawLine(QPointF(x, 16), QPointF(x + 18, 16));

            painter.setPen(QColor(0x40, 0x40, 0x40));
            const QString label = QStringLiteral("CH%1").arg(ch);
            painter.drawText(QPointF(x + 22, 20), label);
            x += 22 + fm.horizontalAdvance(label) + 14;
        }
        if (channelCount_ > kMaxLegendChannels) {
            painter.setPen(QColor(0x60, 0x60, 0x60));
            painter.drawText(QPointF(x, 20),
                             QStringLiteral("… 共 %1 通道").arg(channelCount_));
        }
    }

    // 7) 没有数据时给个提示，而不是空白一片。
    if (!hasData_) {
        painter.setPen(QColor(0x90, 0x90, 0x90));
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("等待设备数据..."));
        return;
    }

    // 8) 画曲线：每通道一条折线，点已按像素桶聚合
    painter.save();
    painter.setClipRect(plot);

    const double valueRange = kValueMax - kValueMin;
    for (int ch = 0; ch < channelCount_; ++ch) {
        const auto& points = channels_[static_cast<std::size_t>(ch)];
        if (points.size() < 2)
            continue;

        QPolygonF polyline;
        polyline.reserve(static_cast<int>(points.size()));

        for (const QPointF& p : points) {
            if (p.x() < tStart)
                continue;

            const double pxRaw = plot.left() + (p.x() - tStart) / windowSec_ * plot.width();
            const double pyRaw =
                plot.top() + (1.0 - (p.y() - kValueMin) / valueRange) * plot.height();

            // 钳制坐标：Qt 光栅化遇到极大坐标会退化甚至长时间卡住。
            const double px = std::clamp(pxRaw, plot.left() - 8.0, plot.right() + 8.0);
            const double py = std::clamp(pyRaw, plot.top() - 8.0, plot.bottom() + 8.0);
            polyline.append(QPointF(px, py));
        }

        if (polyline.size() < 2)
            continue;

        QPen pen(channelColor(ch));
        // 关键性能点：宽度必须为 1 且 cosmetic，才会走快速整数画线；
        // 非整数宽度会触发 QStroker 矢量描边，性能差几个数量级。
        pen.setWidth(1);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawPolyline(polyline);
    }

    painter.restore();

    // 9) 规格限/报警阈值：按通道叠加虚线，现场不用去翻配置。
    {
        const double valueRange = kValueMax - kValueMin;
        auto yOf = [&](double value) {
            return plot.top()
                + (1.0 - (value - kValueMin) / valueRange) * plot.height();
        };
        for (int ch = 0; ch < limits_.size() && ch < channelCount_; ++ch) {
            const ChannelLimit& limit = limits_.at(ch);
            if (!limit.hasHigh && !limit.hasLow)
                continue;
            QColor color = channelColor(ch);
            color.setAlpha(160);
            QPen pen(color);
            pen.setStyle(Qt::DashLine);
            pen.setWidth(1);
            pen.setCosmetic(true);
            painter.setPen(pen);
            if (limit.hasHigh) {
                const double y = yOf(limit.high);
                if (y >= plot.top() && y <= plot.bottom()) {
                    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
                    painter.drawText(QPointF(plot.right() - 54, y - 2),
                                     QStringLiteral("%1 H").arg(limit.name));
                }
            }
            if (limit.hasLow) {
                const double y = yOf(limit.low);
                if (y >= plot.top() && y <= plot.bottom()) {
                    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
                    painter.drawText(QPointF(plot.right() - 54, y - 2),
                                     QStringLiteral("%1 L").arg(limit.name));
                }
            }
        }
    }

    // 10) 鼠标游标：竖线 + 时刻 + 各通道读数（只显示前 8 个通道）。
    if (cursorActive_ && hasData_ && plot.width() > 1) {
        const double ratio =
            std::clamp((cursorX_ - plot.left()) / plot.width(), 0.0, 1.0);
        const double tCursor = tStart + ratio * windowSec_;

        QPen cursorPen(QColor(0x34, 0x49, 0x5E));
        cursorPen.setStyle(Qt::DashLine);
        cursorPen.setWidth(1);
        painter.setPen(cursorPen);
        painter.drawLine(QPointF(cursorX_, plot.top()), QPointF(cursorX_, plot.bottom()));
        painter.drawText(QRectF(cursorX_ - 30, plot.bottom() + 2, 60, kBottomMargin - 2),
                         Qt::AlignHCenter | Qt::AlignTop,
                         QStringLiteral("%1s").arg(tCursor, 0, 'f', 2));

        // 时间→值：在已聚合的点上做二分查找，取最近点。
        QStringList lines;
        const int show = std::min(channelCount_, kMaxLegendChannels);
        for (int ch = 0; ch < show; ++ch) {
            const double value = valueNear(channels_[static_cast<std::size_t>(ch)], tCursor);
            if (std::isnan(value))
                continue;
            lines << QStringLiteral("CH%1  %2")
                         .arg(ch)
                         .arg(QString::number(static_cast<int>(std::lround(value))));
        }

        if (!lines.isEmpty()) {
            const QFontMetrics fm(painter.font());
            int boxWidth = 0;
            for (const QString& line : lines)
                boxWidth = std::max(boxWidth, fm.horizontalAdvance(line));
            const double boxHeight = lines.size() * (fm.height() + 2) + 8;
            const QRectF box(plot.right() - boxWidth - 20, plot.top() + 8,
                             boxWidth + 12, boxHeight);
            painter.setPen(QPen(QColor(0xC8, 0xD0, 0xD8)));
            painter.setBrush(QColor(255, 255, 255, 230));
            painter.drawRoundedRect(box, 4, 4);
            painter.setPen(QColor(0x2C, 0x3E, 0x50));
            double y = box.top() + 5;
            for (const QString& line : lines) {
                painter.drawText(QRectF(box.left() + 6, y, box.width() - 8, fm.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter, line);
                y += fm.height() + 2;
            }
        }
    }

    // 11) 双游标测量：左键 A、右键 B，显示 ΔT 与前 8 通道的 ΔV。
    if (hasData_ && plot.width() > 1 && (cursorAX_ >= 0.0 || cursorBX_ >= 0.0)) {
        const QColor colorA(0xD3, 0x54, 0x00);
        const QColor colorB(0x1F, 0x61, 0x8D);
        QFontMetrics fm(painter.font());

        auto drawCursor = [&](double pixel, const QColor& color, const QString& tag) {
            const double x = std::clamp(pixel, plot.left(), plot.right());
            QPen pen(color);
            pen.setWidth(1);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));

            // 顶部用小标签标出 A/B，避免与悬停游标混淆。
            const QString label = tag;
            const double labelWidth = fm.horizontalAdvance(label) + 8;
            const QRectF badge(x - labelWidth / 2.0, plot.top() - 14, labelWidth, 13);
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRoundedRect(badge, 3, 3);
            painter.setPen(QColor(Qt::white));
            painter.drawText(badge, Qt::AlignCenter, label);
            painter.setBrush(Qt::NoBrush);

            const double ratio = std::clamp((x - plot.left()) / plot.width(), 0.0, 1.0);
            const double time = tStart + ratio * windowSec_;
            painter.setPen(color);
            painter.drawText(QRectF(x - 30, plot.bottom() + 2, 60, kBottomMargin - 2),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QString::number(time, 'f', 2));
            return time;
        };

        double timeA = std::numeric_limits<double>::quiet_NaN();
        double timeB = std::numeric_limits<double>::quiet_NaN();
        if (cursorAX_ >= 0.0)
            timeA = drawCursor(cursorAX_, colorA, QStringLiteral("A"));
        if (cursorBX_ >= 0.0)
            timeB = drawCursor(cursorBX_, colorB, QStringLiteral("B"));

        if (!std::isnan(timeA) && !std::isnan(timeB)) {
            QStringList lines;
            lines << QStringLiteral("ΔT  %1 s").arg(std::abs(timeB - timeA), 0, 'f', 4);

            const int show = std::min(channelCount_, kMaxLegendChannels);
            for (int ch = 0; ch < show; ++ch) {
                const auto& points = channels_[static_cast<std::size_t>(ch)];
                const double valueA = valueNear(points, timeA);
                const double valueB = valueNear(points, timeB);
                if (std::isnan(valueA) || std::isnan(valueB))
                    continue;
                lines << QStringLiteral("CH%1  A=%2 B=%3 Δ=%4")
                             .arg(ch)
                             .arg(static_cast<int>(std::lround(valueA)))
                             .arg(static_cast<int>(std::lround(valueB)))
                             .arg(static_cast<int>(std::lround(valueB - valueA)));
            }

            int boxWidth = 0;
            for (const QString& line : lines)
                boxWidth = std::max(boxWidth, fm.horizontalAdvance(line));
            const double boxHeight = lines.size() * (fm.height() + 2) + 8;
            // 放在绘图区左上，与悬停读数的右上角面板错开。
            const QRectF box(plot.left() + 8, plot.top() + 8, boxWidth + 12, boxHeight);
            painter.setPen(QPen(QColor(0xC8, 0xD0, 0xD8)));
            painter.setBrush(QColor(255, 255, 255, 235));
            painter.drawRoundedRect(box, 4, 4);

            double y = box.top() + 5;
            for (int i = 0; i < lines.size(); ++i) {
                painter.setPen(i == 0 ? QColor(0x2C, 0x3E, 0x50) : QColor(0x40, 0x40, 0x40));
                painter.drawText(QRectF(box.left() + 6, y, box.width() - 8, fm.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
                y += fm.height() + 2;
            }
        }
    }
}

} // namespace aegis::ui
