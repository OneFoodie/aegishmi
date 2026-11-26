#pragma once // 头文件保护

#include <QString>
#include <QVector>

namespace aegis::test {

// 方案库中的一条（扫描 configs/flows 得到）。
struct FlowEntry {
    QString path;        // 绝对路径
    QString id;
    QString name;
    QString product;
    QString description;
    QString version;     // 方案版本号（TP-03）
    QString checksum;    // 记录中的校验和（用于界面展示/追溯）
    int steps = 0;
    bool valid = false;  // 加载/校验是否通过
    QString error;       // 无效时的原因
};

// =============================================================================
// FlowCatalog —— 测试方案库
//
// 只做"发现与摘要"，不负责执行；工单绑定方案后用路径直接加载。
// 目录搜索顺序：AEGIS_FLOW_DIR → exe/configs/flows → exe/../../configs/flows。
// =============================================================================
class FlowCatalog {
public:
    static QString defaultDirectory();
    static QVector<FlowEntry> scan(const QString& directory);
    static FlowEntry describe(const QString& path);
};

} // namespace aegis::test
