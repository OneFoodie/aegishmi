#pragma once // 头文件保护

#include "test/flow_types.h"
#include "test/test_device.h"
#include "test/test_result.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariant>

#include <atomic>
#include <functional>

namespace aegis::test {

// 插件调用配置（来自 station.json 的 plugins 段）。
struct PluginContext {
    bool enabled = false;      // 是否允许调用插件
    QString root;              // 插件根目录（相对路径按程序目录解析）
    QStringList allowed;       // 白名单（plugins.enabled）
    int defaultTimeoutMs = 5000;
};

// =============================================================================
// TestEngine —— 测试流程执行引擎
//
// 执行模型：
//   - 同步执行（run() 返回时整个流程跑完）；M3.3 再把它放进工作线程
//   - 按步骤顺序执行，每步可能有重试
//   - 失败时按 onFail / onFailure 决定"终止"还是"继续"
//   - 通过信号汇报进度，界面据此刷新表格
//
// 线程安全：
//   - run() 只在一个线程调用
//   - requestStop() 可以被其他线程调用（例如界面点"停止"）
// =============================================================================
class TestEngine : public QObject {
    Q_OBJECT

public:
    TestEngine(ITestDevice* device, ITestClock* clock, QObject* parent = nullptr);

    // 执行整个流程并返回结果（同时发出 runFinished 信号）。
    RunResult run(const TestFlow& flow);

    // 调试：只执行 flow.steps[index] 一步（含重试），发出 stepStarted / stepFinished。
    // 不发 runFinished，避免界面把单步调试当成一次正式运行去落库/统计。
    StepResult runSingleStep(const TestFlow& flow, int index);

    // 请求停止：当前步骤执行完后中断，剩余步骤标记为中止。
    void requestStop();

    // 插件配置（station.json plugins）与运行上下文（SN/工单等，传给插件）。
    void setPluginContext(const PluginContext& context) { plugins_ = context; }
    void setRunContext(const QJsonObject& context) { runContext_ = context; }

    // TP-14：操作员确认处理器。引擎不直接弹窗（可能在工作线程），
    // 由界面注入实现（例如转到 GUI 线程显示对话框）。返回 false 表示取消/超时。
    using PromptHandler = std::function<bool(const QString& text, int timeoutMs)>;
    void setPromptHandler(PromptHandler handler) { promptHandler_ = std::move(handler); }

signals:
    void stepStarted(int index);
    void stepFinished(int index, const aegis::test::StepResult& result);
    void runFinished(const aegis::test::RunResult& result);

private:
    // 执行单个步骤（不含重试逻辑）。
    StepResult executeStep(const TestStep& step, int index, int timeoutMs,
                           const QString& baseDir, int depth);

    // 核心执行循环。emitProgress 为 false 时用于子流程（不刷新主表格）。
    RunResult runFlow(const TestFlow& flow, int depth, bool emitProgress);

    // TP-12a：展开并执行子流程，把结果聚合成一个步骤结果。
    StepResult executeSubFlow(const TestStep& step, int index, int timeoutMs,
                              const QString& baseDir, int depth);

    // 条件表达式求值（JS）。last 可为空；返回 false 表示条件不满足。
    // 表达式语法错误时通过 error 返回原因（此时返回值无意义）。
    bool evaluateCondition(const QString& expression, const StepResult* last,
                           QString* error) const;

    ITestDevice* device_ = nullptr;
    ITestClock* clock_ = nullptr;
    PluginContext plugins_;
    QJsonObject runContext_;
    // 运行期变量：流程 variables 的副本，脚本可写，条件表达式可读。
    QHash<QString, QVariant> runtimeVariables_;
    PromptHandler promptHandler_;
    std::atomic<bool> stopRequested_{false};
};

} // namespace aegis::test
