#pragma once // 头文件保护

#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;

namespace aegis::service {
class DeviceManager;
}

namespace aegis::storage {
class TestRepository;
}

namespace aegis::ui {

class TrendView;

// =============================================================================
// TrendPage —— 历史趋势页（P3）
//
// 数据源：sample_summary（每秒每通道一行 min/max/avg/count），
// 与实时波形解耦，因此几小时的数据也只需几千行查询。
//
// 交互：选设备 / 通道 / 时间范围 → 刷新，趋势图显示 min-max 包络与 avg 曲线。
// =============================================================================
class TrendPage : public QWidget {
    Q_OBJECT

public:
    TrendPage(storage::TestRepository* repository, service::DeviceManager* manager,
              QWidget* parent = nullptr);

public slots:
    void refresh();

private:
    void buildUi();
    void populateChannels(const QString& deviceId);

    storage::TestRepository* repository_ = nullptr;
    service::DeviceManager* manager_ = nullptr;

    QComboBox* deviceCombo_ = nullptr;
    QListWidget* channelList_ = nullptr; // 可勾选通道（多曲线对比，最多 4 条）
    QComboBox* rangeCombo_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    TrendView* view_ = nullptr;
};

} // namespace aegis::ui
