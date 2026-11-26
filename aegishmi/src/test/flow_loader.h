#pragma once // 头文件保护

#include "test/flow_types.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace aegis::test {

// =============================================================================
// FlowLoader —— 把 JSON 配置读成 TestFlow
//
// 职责：
//   1. 解析 JSON（语法错误给出偏移量）
//   2. 读取 variables 并做 ${var} 替换（替换失败记错误，不静默）
//   3. 组装领域对象
//
// 注意：加载器只负责"读得出来"，不负责业务规则校验（那是 FlowValidator 的事）。
//       两者分开的好处：JSON 语法问题与配置语义问题可以分别测试。
// =============================================================================
class FlowLoader {
public:
    struct LoadResult {
        TestFlow flow;
        QVector<FlowIssue> errors;   // 非空表示加载失败
        QVector<FlowIssue> warnings; // 暂未使用，预留

        bool ok() const { return errors.isEmpty(); }
    };

    // 从文件加载；文件不存在/不可读也会转成 errors。
    static LoadResult loadFromFile(const QString& path);

    // 从内存中的 JSON 文本加载（单元测试主要用这个）。
    static LoadResult loadFromJson(const QByteArray& json);
};

} // namespace aegis::test
