#include "quality/spc.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace aegis::quality {

namespace {

// 单值-移动极差控制图常数（子组大小 n=2）：
//   E2 = 2.66（单值图 3σ 限对应 MRbar 的倍数）
//   D4 = 3.267（移动极差图的上控制限系数）
constexpr double kE2 = 2.66;
constexpr double kD4 = 3.267;

} // namespace

SpcResult compute(const QVector<double>& values, bool hasUsl, double usl,
                  bool hasLsl, double lsl)
{
    SpcResult result;
    result.values = values;
    result.n = values.size();
    result.hasUsl = hasUsl;
    result.hasLsl = hasLsl;
    result.usl = usl;
    result.lsl = lsl;
    result.hasLimits = hasUsl || hasLsl;

    if (values.isEmpty())
        return result;

    double sum = 0.0;
    result.minValue = values.first();
    result.maxValue = values.first();
    for (double value : values) {
        sum += value;
        result.minValue = std::min(result.minValue, value);
        result.maxValue = std::max(result.maxValue, value);
    }
    result.mean = sum / values.size();

    if (values.size() < 2)
        return result;

    // 样本标准差（n-1）：估计过程总体波动。
    double variance = 0.0;
    for (double value : values) {
        const double diff = value - result.mean;
        variance += diff * diff;
    }
    result.sigma = std::sqrt(variance / (values.size() - 1));

    // 移动极差：相邻两点差值绝对值。
    double mrSum = 0.0;
    result.movingRanges.reserve(values.size() - 1);
    for (int i = 1; i < values.size(); ++i) {
        const double range = std::abs(values.at(i) - values.at(i - 1));
        result.movingRanges.append(range);
        mrSum += range;
    }
    result.mrBar = mrSum / result.movingRanges.size();

    // 控制限：MRbar 为 0（数据完全不变）时控制限退化为均值线，避免所有点越限。
    const double spread = kE2 * result.mrBar;
    result.uclX = result.mean + spread;
    result.lclX = result.mean - spread;
    result.uclMr = kD4 * result.mrBar;

    // ---- 过程能力指数 ----
    if (result.sigma > 0.0) {
        if (hasUsl && hasLsl && usl > lsl) {
            result.hasCp = true;
            result.cp = (usl - lsl) / (6.0 * result.sigma);
        }

        bool hasCpkSide = false;
        double cpk = 0.0;
        if (hasUsl) {
            cpk = (usl - result.mean) / (3.0 * result.sigma);
            hasCpkSide = true;
        }
        if (hasLsl) {
            const double lower = (result.mean - lsl) / (3.0 * result.sigma);
            cpk = hasCpkSide ? std::min(cpk, lower) : lower;
            hasCpkSide = true;
        }
        if (hasCpkSide) {
            result.hasCpk = true;
            result.cpk = cpk;
        }
    }

    // ---- 判异规则（Nelson 常用子集）----
    // 1) 越出控制限
    for (int i = 0; i < values.size(); ++i) {
        if (values.at(i) > result.uclX || values.at(i) < result.lclX) {
            result.outOfControl.append(i);
            result.violations.append(
                QStringLiteral("第 %1 点越出控制限（%2）")
                    .arg(i + 1)
                    .arg(values.at(i), 0, 'f', 2));
        }
    }

    // 2) 连续 7 点在中心线同侧
    int runStart = 0;
    for (int i = 1; i <= values.size(); ++i) {
        const bool end = i == values.size();
        const bool sameSide = !end
            && ((values.at(i) > result.mean) == (values.at(runStart) > result.mean));
        const bool sameValue = !end && values.at(i) == result.mean;
        if (end || !sameSide || sameValue) {
            const int length = i - runStart;
            if (length >= 7 && values.at(runStart) != result.mean) {
                result.violations.append(
                    QStringLiteral("第 %1~%2 点连续 %3 点在中心线同一侧")
                        .arg(runStart + 1)
                        .arg(i)
                        .arg(length));
            }
            runStart = i;
        }
    }

    // 3) 连续 6 点单调上升/下降
    int trendStart = 0;
    int direction = 0; // 1 上升，-1 下降
    for (int i = 1; i < values.size(); ++i) {
        const double delta = values.at(i) - values.at(i - 1);
        const int current = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
        if (current == 0) {
            trendStart = i;
            direction = 0;
            continue;
        }
        if (direction == 0) {
            direction = current;
            trendStart = i - 1;
            continue;
        }
        if (current != direction) {
            const int length = i - trendStart;
            if (length >= 6) {
                result.violations.append(
                    QStringLiteral("第 %1~%2 点连续 %3 点%4")
                        .arg(trendStart + 1)
                        .arg(i)
                        .arg(length)
                        .arg(direction > 0 ? QStringLiteral("上升")
                                           : QStringLiteral("下降")));
            }
            direction = current;
            trendStart = i - 1;
        }
    }
    // 收尾：最后一段趋势也要判。
    {
        const int length = values.size() - trendStart;
        if (direction != 0 && length >= 6) {
            result.violations.append(
                QStringLiteral("第 %1~%2 点连续 %3 点%4")
                    .arg(trendStart + 1)
                    .arg(values.size())
                    .arg(length)
                    .arg(direction > 0 ? QStringLiteral("上升")
                                       : QStringLiteral("下降")));
        }
    }

    // 4) 连续 3 点中至少 2 点超出 2σ（同侧）
    if (result.sigma > 0.0) {
        const double upper2 = result.mean + 2.0 * result.sigma;
        const double lower2 = result.mean - 2.0 * result.sigma;
        for (int i = 0; i + 2 < values.size(); ++i) {
            int above = 0;
            int below = 0;
            for (int k = 0; k < 3; ++k) {
                const double value = values.at(i + k);
                if (value > upper2)
                    ++above;
                if (value < lower2)
                    ++below;
            }
            if (above >= 2 || below >= 2) {
                result.violations.append(
                    QStringLiteral("第 %1~%2 点中有 2 点以上超出 2σ（%3）")
                        .arg(i + 1)
                        .arg(i + 3)
                        .arg(above >= 2 ? QStringLiteral("上侧")
                                        : QStringLiteral("下侧")));
            }
        }
    }

    return result;
}

QString gradeForCpk(double cpk, bool hasCpk)
{
    if (!hasCpk)
        return QStringLiteral("—");
    if (cpk >= 1.67)
        return QStringLiteral("A+");
    if (cpk >= 1.33)
        return QStringLiteral("A");
    if (cpk >= 1.0)
        return QStringLiteral("B");
    if (cpk >= 0.67)
        return QStringLiteral("C");
    return QStringLiteral("D");
}

} // namespace aegis::quality
