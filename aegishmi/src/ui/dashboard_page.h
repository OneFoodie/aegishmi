#pragma once // 头文件保护

#include "core/station_config.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
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
// DashboardPage —— 生产看板
//
// 面向产线工位的首屏：
//   - 选择/新建/关闭工单；
//   - 产量、PASS/FAIL、FPY（直通率）、UPH、MES 待传/失败；
//   - 不良步骤 Pareto、最近测试记录。
// =============================================================================
class DashboardPage : public QWidget {
    Q_OBJECT

public:
    DashboardPage(storage::TestRepository* repository,
                  security::AuthService* auth,
                  const core::StationConfig& station,
                  QWidget* parent = nullptr);

    void refresh();

    QString activeOrderNo() const { return activeOrderNo_; }
    QString activeProduct() const { return activeProduct_; }

    // 指定当前工单；notify 为 true 时发出 activeOrderChanged。
    void setActiveOrder(const QString& orderNo, bool notify = true);

    // 测试执行期间禁止切换/新建/关闭工单。
    void setOrderSelectionEnabled(bool enabled);

    // 观察员只读：禁止新建/关闭工单与 MES 重传。
    void setOperationAllowed(bool allowed);

signals:
    void activeOrderChanged(const QString& orderNo, const QString& product);
    void mesRetryRequested();

private slots:
    void createOrder();
    void closeOrder();
    void confirmFai();
    void resetFai();
    void resetFixture();
    void clearAndon();
    void importSnList();
    void exportDailyReport();
    void onOrderChanged(int index);

private:
    void refreshOrderList();
    void refreshStats();
    void updateOrderInfo();
    void refreshAndon();    // 刷新区停线横幅与"解除"按钮
    void applyControlState(); // 汇总"角色权限 + 是否运行中 + MES 失败数"
    // 关键操作双人复核：弹窗校验第二人账号口令，成功返回复核人。
    bool requireSecondApproval(const QString& action,
                               QString* approverName = nullptr);

    storage::TestRepository* repository_ = nullptr;
    security::AuthService* auth_ = nullptr;
    core::StationConfig station_;

    QComboBox* orderCombo_ = nullptr;
    QPushButton* newButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* mesRetryButton_ = nullptr;
    QPushButton* faiConfirmButton_ = nullptr;
    QPushButton* faiResetButton_ = nullptr;
    QPushButton* fixtureResetButton_ = nullptr;
    QPushButton* snImportButton_ = nullptr;
    QPushButton* reportButton_ = nullptr;

    QLabel* stationLabel_ = nullptr;
    QLabel* andonBanner_ = nullptr;
    QPushButton* andonClearButton_ = nullptr;
    QLabel* orderInfo_ = nullptr;
    QLabel* fixtureLabel_ = nullptr;
    QLabel* progressText_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* totalValue_ = nullptr;
    QLabel* passValue_ = nullptr;
    QLabel* failValue_ = nullptr;
    QLabel* fpyValue_ = nullptr;
    QLabel* uphValue_ = nullptr;
    QLabel* mesValue_ = nullptr;
    QLabel* retestPassValue_ = nullptr;
    QLabel* pendingRepairValue_ = nullptr;
    QTableWidget* paretoTable_ = nullptr;
    QTableWidget* paretoReasonTable_ = nullptr;
    QTableWidget* slowTable_ = nullptr;
    QTableWidget* recentTable_ = nullptr;

    QString activeOrderNo_;
    QString activeProduct_;
    bool updatingCombo_ = false;
    bool operationAllowed_ = true;   // 观察员为 false
    bool selectionEnabled_ = true;   // 测试运行中为 false
    int deadCount_ = 0;              // MES 失败（DEAD）数量
    bool canConfirmFai_ = false;     // 当前工单处于 WAITING
    bool canResetFai_ = false;       // 当前工单处于 REJECTED
    bool andonLocked_ = false;       // 工位处于停线状态
};

} // namespace aegis::ui
