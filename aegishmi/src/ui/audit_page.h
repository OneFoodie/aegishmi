#pragma once // 头文件保护

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace aegis::security {
class AuthService;
}

namespace aegis::storage {
class SystemRepository;
}

namespace aegis::ui {

// =============================================================================
// AuditPage —— 操作审计
//
// 只读表格：时间 / 用户 / 动作 / 对象 / 结果 / 详情。
// 数据量按"最近 500 条"限制，切到本页或点刷新时重新查询。
// 支持导出 CSV（UTF-8 BOM），导出动作本身也写审计。
// =============================================================================
class AuditPage : public QWidget {
    Q_OBJECT

public:
    AuditPage(storage::SystemRepository* repository, security::AuthService* auth,
              QWidget* parent = nullptr);

    void refresh();

private slots:
    void exportCsv();
    void backupDatabase();
    void vacuumDatabase();

private:
    storage::SystemRepository* repository_ = nullptr;
    security::AuthService* auth_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* info_ = nullptr;
};

} // namespace aegis::ui
