#pragma once // 头文件保护

#include "core/station_config.h"

#include <QWidget>

class QLabel;
class QPushButton;
class QTableWidget;

namespace aegis::security {
class AuthService;
}

namespace aegis::storage {
class TestRepository;
}

namespace aegis::ui {

// =============================================================================
// ShiftPage —— 班组与交接班
//
//   - 当前班次（按 station.json 的 shifts 判定）与操作员资质；
//   - 交接班：记录当前工单的产量/通过/失败快照与备注，写审计；
//   - 最近交接班列表（可追溯谁在什么时候把工位交给了谁）。
// =============================================================================
class ShiftPage : public QWidget {
    Q_OBJECT

public:
    ShiftPage(storage::TestRepository* repository, security::AuthService* auth,
              const core::StationConfig& station, QWidget* parent = nullptr);

    // 主界面切换工单时同步（交接班记录绑定当前工单）。
    void setActiveOrder(const QString& orderNo, const QString& product);

    void setOperationAllowed(bool allowed);

    void refresh();

private slots:
    void handover();

private:
    void applyControlState();

    storage::TestRepository* repository_ = nullptr;
    security::AuthService* auth_ = nullptr;
    core::StationConfig station_;

    QString orderNo_;
    QString product_;
    bool operationAllowed_ = true;

    QLabel* shiftLabel_ = nullptr;
    QLabel* qualificationLabel_ = nullptr;
    QTableWidget* qualificationTable_ = nullptr;
    QPushButton* handoverButton_ = nullptr;
    QTableWidget* historyTable_ = nullptr;
};

} // namespace aegis::ui
