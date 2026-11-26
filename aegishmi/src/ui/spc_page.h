#pragma once // 头文件保护

#include <QWidget>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace aegis::storage {
class TestRepository;
}

namespace aegis::ui {

class ControlChart; // 自绘控制图（见 spc_page.cpp）

// =============================================================================
// SpcPage —— 统计过程控制（SPC）
//
// 按工单选择测试项，展示：
//   - 过程能力：n / 均值 / σ / Cp / Cpk / 等级
//   - 单值-移动极差（I-MR）控制图：中心线、控制限、规格限
//   - 判异规则命中列表与最近数据点
// =============================================================================
class SpcPage : public QWidget {
    Q_OBJECT

public:
    explicit SpcPage(storage::TestRepository* repository, QWidget* parent = nullptr);

    void refresh();

private slots:
    void onOrderChanged();
    void onItemChanged();

private:
    void refreshOrders();
    void refreshItems();
    void updateChart();

    storage::TestRepository* repository_ = nullptr;

    QComboBox* orderCombo_ = nullptr;
    QComboBox* itemCombo_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* specLabel_ = nullptr;
    QLabel* metrics_ = nullptr;
    ControlChart* chart_ = nullptr;
    QTableWidget* valuesTable_ = nullptr;
    QPlainTextEdit* violations_ = nullptr;
};

} // namespace aegis::ui
