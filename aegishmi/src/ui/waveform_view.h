#pragma once // 头文件保护

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QVector>
#include <QWidget>

#include <cstdint>
#include <deque>
#include <vector>

class QContextMenuEvent;
class QMouseEvent;
class QWheelEvent;

namespace aegis::ui {

// 通道规格限/报警阈值（叠加显示在波形上，避免现场只看曲线没有判据）。
struct ChannelLimit {
    QString name;
    bool hasHigh = false;
    double high = 0.0;
    bool hasLow = false;
    double low = 0.0;
};

// =============================================================================
// WaveformView —— 自绘实时波形控件（通道数可变 + 像素级 min/max 抽稀）
//
// 为什么不用 Qt Charts？
//   QChart 每次数据变化都会重绘坐标轴、图例、网格、曲线，10kHz 多通道下会占满 CPU。
//   工业上位机通常"自绘 + 只画屏幕需要的点"。
//
// 抽稀策略（P1）：
//   显示器横向只有几百个像素，10kHz × 32 通道直接画会浪费 99% 的点。
//   这里按"每个像素一个时间桶"聚合：桶内保存最小值和最大值，
//   绘制时每桶输出两个点（谷/峰），既保留毛刺又能把点数压到 2×像素数。
//
// 线程：只在 UI 线程调用（onTick 消费 Chunk 时）。
// =============================================================================
class WaveformView : public QWidget {
    Q_OBJECT

public:
    explicit WaveformView(QWidget* parent = nullptr);

    // 追加一块交叠采样数据（pointCount 个时刻 × channelCount 个通道）。
    void appendChunk(double startSec, const std::int16_t* interleaved,
                     int pointCount, int channelCount, int sampleRateHz);

    // 设置横轴显示窗口长度（秒）。
    void setWindowSeconds(double seconds);

    // 叠加显示通道上限/下限（虚线）。空列表表示不显示。
    void setChannelLimits(const QVector<ChannelLimit>& limits);

    // 清空所有曲线数据（断线重连时调用）。
    void clearData();

protected:
    // Qt 的重绘入口：窗口需要刷新时由框架调用。
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // 通道数变化时重建内部容器。
    void ensureChannels(int channelCount);

    // 把当前时间桶的 min/max 写入各通道曲线。
    void flushBucket();

    // 通道配色：前 8 个用固定色板，更多通道用 HSV 生成。
    QColor channelColor(int channel) const;

    // 每通道最多保留的点数（2 点/桶 × 约 2000 桶）。
    static constexpr int kMaxPointsPerChannel = 4096;

    // 纵轴固定范围（int16 采样值域是 -32768~32767，这里放大显示细节）。
    static constexpr double kValueMin = -15000.0;
    static constexpr double kValueMax = 15000.0;

    std::vector<std::deque<QPointF>> channels_; // 每通道的 (时间, 值) 点序列
    std::vector<double> bucketMin_;             // 当前桶内各通道最小值
    std::vector<double> bucketMax_;             // 当前桶内各通道最大值

    int channelCount_ = 0;      // 当前通道数（0 表示还没有数据）
    double windowSec_ = 3.0;    // 横轴窗口长度
    double bucketSec_ = 0.003;  // 每个时间桶的长度（随控件宽度变化）
    qint64 currentBucket_ = -1; // 当前桶索引；-1 表示尚未开始
    double lastTimeSec_ = 0.0;  // 最新数据时刻
    bool hasData_ = false;      // 是否已有数据（用于显示提示文字）

    // ---- 规格限与游标（P1-4）----
    QVector<ChannelLimit> limits_;
    QRectF lastPlot_;           // 最近一次绘图的绘图区（游标换算用）
    double lastTStart_ = 0.0;
    double cursorX_ = -1.0;     // 悬停游标 x 像素；<0 表示隐藏
    bool cursorActive_ = false;

    // ---- 双游标（A/B）：左键定位 A、右键定位 B，双击清除 ----
    double cursorAX_ = -1.0;    // A 游标 x 像素；<0 表示未定位
    double cursorBX_ = -1.0;    // B 游标 x 像素；<0 表示未定位
};

} // namespace aegis::ui
