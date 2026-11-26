#pragma once // 头文件保护

#include "test/flow_types.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace aegis::test {

// =============================================================================
// FlowWriter —— 把 TestFlow 序列化回 JSON
//
// 与 FlowLoader 配对：FlowWriter::toJson(load(json)) 再次加载应得到等价流程，
// 保证图形化编辑器保存后测试引擎仍能直接执行。
// =============================================================================
class FlowWriter {
public:
    // action / metric 的 JSON 文本（与 FlowLoader 的解析一一对应）。
    static QString actionText(StepAction action);
    static QString metricText(Metric metric);

    // 组装单个步骤的 JSON（含 params 与 expect）。
    static QJsonObject stepToJson(const TestStep& step);

    // 组装完整流程的 JSON 对象。
    static QJsonObject toJson(const TestFlow& flow);

    // 带缩进格式的 UTF-8 文本。
    static QByteArray toJsonBytes(const TestFlow& flow);

    // 写入文件（UTF-8，缩进 4 空格）；失败时通过 error 返回原因。
    static bool saveToFile(const TestFlow& flow, const QString& path, QString* error = nullptr);
};

} // namespace aegis::test
