#pragma once // 头文件保护

#include <QString>
#include <QStringList>
#include <QVector>

namespace aegis::quality {

// =============================================================================
// SPC（统计过程控制）计算
//
// 输入是同一测试项（measure 步骤）在若干产品上的实测值，以及该步骤的
// 规格上下限（USL/LSL，可选）。输出过程能力指数与单值-移动极差（I-MR）
// 控制图参数，并给出常见判异规则命中的位置。
//
// 纯函数、不依赖数据库与界面，便于单元测试。
// =============================================================================
struct SpcResult {
    int n = 0;              // 样本数
    double mean = 0.0;      // 均值
    double sigma = 0.0;     // 样本标准差（n-1）
    double minValue = 0.0;
    double maxValue = 0.0;

    bool hasLimits = false; // 是否配置了规格限
    bool hasUsl = false;
    bool hasLsl = false;
    double usl = 0.0;
    double lsl = 0.0;

    bool hasCp = false;     // 双侧规格才计算 Cp
    bool hasCpk = false;
    double cp = 0.0;
    double cpk = 0.0;

    // ---- I-MR 控制图 ----
    double mrBar = 0.0;     // 移动极差均值
    double uclX = 0.0;      // 单值图上控制限 = mean + 2.66 * MRbar
    double lclX = 0.0;      // 单值图下控制限 = mean - 2.66 * MRbar
    double uclMr = 0.0;     // 移动极差上控制限 = 3.267 * MRbar

    QVector<double> values;       // 按时间升序的实测值
    QVector<double> movingRanges; // 相邻两点移动极差（比 values 少 1 个）
    QVector<int> outOfControl;    // 越出单值控制限的下标
    QStringList violations;       // 判异说明（规则 + 位置）
};

// 计算过程能力与控制图参数。
//   values 为时间升序实测值；hasUsl/hasLsl 为假时对应规格限不参与判定。
SpcResult compute(const QVector<double>& values, bool hasUsl, double usl,
                  bool hasLsl, double lsl);

// 过程能力等级（A+/A/B/C/D），未计算 Cpk 时返回 "—"。
QString gradeForCpk(double cpk, bool hasCpk);

} // namespace aegis::quality
