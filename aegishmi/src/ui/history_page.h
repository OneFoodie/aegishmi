#pragma once // 头文件保护

#include "core/station_config.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
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
// HistoryPage —— 历史测试记录
//
// 上半部分：运行列表（时间/方案/产品/SN/判定/耗时）
// 下半部分：选中运行的步骤明细
// 工具栏：刷新、导出 CSV
// =============================================================================
class HistoryPage : public QWidget {
    Q_OBJECT

public:
    explicit HistoryPage(storage::TestRepository* repository,
                         security::AuthService* auth,
                         const core::StationConfig& station,
                         QWidget* parent = nullptr);

    // 重新查询数据库并刷新列表（切换页签时调用）。
    void refresh();

    // 观察员只读：禁用导出。
    void setExportAllowed(bool allowed);

private slots:
    void exportSelected();
    void exportPdf();
    void exportBatchPdf();
    void onRunSelected();

private:
    void buildUi();

    storage::TestRepository* repository_ = nullptr;
    security::AuthService* auth_ = nullptr;
    core::StationConfig station_;
    QTableWidget* runTable_ = nullptr;
    QTableWidget* stepTable_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QPushButton* pdfButton_ = nullptr;
    QPushButton* batchPdfButton_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QLineEdit* searchEdit_ = nullptr;
    QComboBox* verdictCombo_ = nullptr;
    int currentRunId_ = -1;
    bool exportAllowed_ = true;
};

} // namespace aegis::ui
