#pragma once // 头文件保护

#include "core/station_config.h"
#include "test/plugin_runner.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace aegis::security {
class AuthService;
}

namespace aegis::ui {

// =============================================================================
// PluginPage —— 插件管理页
//
// 目标：把"插件"从只有配置文件的知识变成可见、可验证、可复用的功能：
//   - 扫描 plugins/ 下所有 manifest.json，展示版本/API/能力/状态；
//   - 选中插件查看详情与目录位置；
//   - 一键自检（用示例上下文调用插件），确认协议连通；
//   - 打开插件目录，便于整包复制到其它工位。
// =============================================================================
class PluginPage : public QWidget {
    Q_OBJECT

public:
    PluginPage(const core::StationConfig& station, security::AuthService* auth,
               QWidget* parent = nullptr);

    void refresh();
    // 观察员只读：禁用自检。
    void setOperationAllowed(bool allowed);

private slots:
    void runSelfTest();
    void openPluginsDirectory();
    void onSelectionChanged();

private:
    void showDetails(int row);
    QString pluginsRoot() const;
    bool whitelisted(const QString& name) const;

    core::StationConfig station_;
    security::AuthService* auth_ = nullptr;
    QVector<test::PluginInfo> plugins_;

    QTableWidget* table_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QLabel* summary_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* testButton_ = nullptr;
    QPushButton* openButton_ = nullptr;
    bool operationAllowed_ = true;
};

} // namespace aegis::ui
