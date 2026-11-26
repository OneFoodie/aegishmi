#pragma once // 头文件保护

#include "storage/test_repository.h" // SummaryRow

#include <QVector>
#include <QWidget>

namespace aegis::ui {

// 一条趋势曲线（一个通道）。参考 PI Vision/Grafana 的多曲线对比：
// 同一时间轴叠加多个通道，图例标出通道号与颜色。
struct TrendSeries {
    int channel = 0;
    QVector<storage::SummaryRow> rows;
};

// =============================================================================
// TrendView —— 秒级摘要趋势图（min/max 包络 + avg 曲线）
//
// 数据来自 sample_summary 表（1 秒一行），因此点数很少（1 小时 = 3600 行），
// 可以用普通 QPainter 直接画：
//   - 单通道：浅色带显示 [min, max]，深色线为 avg；
//   - 多通道：各通道 avg 曲线叠加，图例显示通道号；
//   - 纵轴自动按数据范围缩放，横轴显示实际时间。
// =============================================================================
class TrendView : public QWidget {
    Q_OBJECT

public:
    explicit TrendView(QWidget* parent = nullptr);

    // 多通道叠加。fromSec/toSec 为查询范围（用于横轴）。
    void setSeries(const QVector<TrendSeries>& series, qint64 fromSec, qint64 toSec);
    // 单通道便捷入口（保留旧调用方式）。
    void setData(const QVector<storage::SummaryRow>& rows, qint64 fromSec, qint64 toSec);
    void clearData();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QVector<TrendSeries> series_;
    qint64 fromSec_ = 0;
    qint64 toSec_ = 0;
};

} // namespace aegis::ui
