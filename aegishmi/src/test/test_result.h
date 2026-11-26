#pragma once // 头文件保护

#include <QMetaType>
#include <QString>
#include <QVector>

namespace aegis::test {

// 单个步骤的执行状态。
enum class StepStatus {
    NotRun,  // 尚未执行
    Pass,    // 通过
    Fail,    // 失败（越界 / 超时 / 应答错误）
    Skipped, // 因前序失败且策略为 abort 而跳过
    Aborted, // 用户停止或断线导致中止
};

// 整次运行的结果。
enum class RunVerdict {
    Pass,    // 全部通过
    Fail,    // 至少一步失败
    Aborted, // 中途停止
};

// 单个步骤的结果明细（界面表格一行）。
struct StepResult {
    int index = -1;                 // 在流程中的序号
    QString id;                     // 步骤 id
    QString name;                   // 步骤名称
    StepStatus status = StepStatus::NotRun;
    double measured = 0.0;          // 实测值（measure 步骤才有意义）
    bool hasMeasured = false;       // 是否产生了实测值
    int elapsedMs = 0;              // 本步耗时
    QString message;                // 失败原因 / 重试说明
};

// 一次完整运行的结果。
struct RunResult {
    RunVerdict verdict = RunVerdict::Aborted;
    QVector<StepResult> steps;
    int totalElapsedMs = 0;

    bool allPassed() const { return verdict == RunVerdict::Pass; }
};

} // namespace aegis::test

// 注册为 Qt 元类型：将来引擎放到工作线程、用队列信号通知界面时需要。
Q_DECLARE_METATYPE(aegis::test::StepResult)
Q_DECLARE_METATYPE(aegis::test::RunResult)
