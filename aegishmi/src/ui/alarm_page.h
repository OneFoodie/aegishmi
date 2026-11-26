#pragma once // 头文件保护

#include "storage/system_repository.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace aegis::security {
class AuthService;
}

namespace aegis::service {
class AlarmMonitor;
}

namespace aegis::ui {

// =============================================================================
// AlarmPage —— 报警列表与确认
//
// 表格：时间 / 设备 / 通道 / 级别 / 实测 / 阈值 / 状态 / 确认人 / 确认时间
// 操作：刷新、确认选中、全部确认（需要"操作"权限）。
// =============================================================================
class AlarmPage : public QWidget {
    Q_OBJECT

public:
    explicit AlarmPage(aegis::storage::SystemRepository* repository,
                       security::AuthService* auth,
                       service::AlarmMonitor* monitor = nullptr,
                       QWidget* parent = nullptr);

    // 重新查询并刷新表格。
    void refresh();

    // 观察员只读：禁用确认按钮。
    void setAckAllowed(bool allowed);

private slots:
    void acknowledgeSelected();
    void acknowledgeAll();
    void shelveSelected();

private:
    int selectedAlarmId() const;
    int selectedRow() const;

    aegis::storage::SystemRepository* repository_ = nullptr;
    security::AuthService* auth_ = nullptr;
    service::AlarmMonitor* monitor_ = nullptr; // 搁置（ISA-18.2）
    QVector<aegis::storage::AlarmRecord> rows_; // 当前表格对应的数据（含确认状态）
    QTableWidget* table_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* ackButton_ = nullptr;
    QPushButton* ackAllButton_ = nullptr;
    QPushButton* shelveButton_ = nullptr;
    bool ackAllowed_ = true;
};

} // namespace aegis::ui
