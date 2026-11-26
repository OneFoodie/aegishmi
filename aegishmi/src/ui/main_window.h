#pragma once // 头文件保护

#include "core/language_manager.h"
#include "core/station_config.h"

#include <QMainWindow> // 主窗口基类
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <array>
#include <cstdint>
#include <vector>

// 前置声明：头文件里只用指针，不需要完整定义，减少编译依赖。
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QListWidget;
class QTableWidget;
class QTabWidget;
class QTimer;
namespace aegis::security {
class AuthService;
}

namespace aegis::service {
class AlarmMonitor;
class DeviceManager;
class DeviceService;
class MesUploader;
class UpdateChecker; // 轻量版本检测（SY-05）
} // namespace aegis::service

namespace aegis::storage {
class SummaryWriter;
class SystemRepository;
class TestRepository;
}

namespace aegis::ui {

class WaveformView; // 自绘波形控件（见 waveform_view.h）
class TestPage;     // 自动测试页（见 test_page.h）
class HistoryPage;  // 历史记录页（见 history_page.h）
class TrendPage;    // 趋势页（见 trend_page.h）
class AlarmPage;    // 报警页（见 alarm_page.h）
class AuditPage;    // 审计页（见 audit_page.h）
class DashboardPage; // 生产看板（见 dashboard_page.h）
class SpcPage;       // SPC 控制图（见 spc_page.h）
class PluginPage;    // 插件管理（见 plugin_page.h）
class ShiftPage;     // 班组与交接班（见 shift_page.h）
class UserPage;      // 用户管理（见 user_page.h）

// =============================================================================
// MainWindow —— 上位机主界面
//
// 界面结构：
//   ┌──────────────┬───────────────────────────────┐
//   │ 连接参数      │                               │
//   │ 状态/统计     │        实时波形（自绘）        │
//   │ 运行日志      │                               │
//   └──────────────┴───────────────────────────────┘
//
// 数据流：
//   DeviceService 收包 -> 环形缓冲 -> 本类定时器批量取出 -> 抽稀 -> WaveformView
// 界面刷新率固定（20fps），与设备上报速率（10kHz）解耦。
// =============================================================================
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(security::AuthService* auth,
                        storage::SystemRepository* systemRepository,
                        QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void toggleConnection();                                        // 连接/断开当前设备
    void onTick();                                                  // 定时刷新界面
    void onDeviceSelected(int row);                                 // 设备列表选择变化
    void onDeviceConnectionChanged(const QString& id, const QString& linkId,
                                   bool connected, const QString& detail); // 某链路连接状态变化
    void onDeviceLog(const QString& id, const QString& linkId,
                     const QString& message);                       // 某链路日志
    void onTestRunningChanged(bool running);                        // 测试开始/结束
    void switchUser();                                              // 切换登录用户
    void toggleWaveformCapture();                                   // 捕获/保存原始波形
    void runDailyCheck();                                           // 通道日常点检
    void onHexLoggingToggled(bool enabled);                         // 报文诊断开关

private:
    void buildUi();                        // 组装页签
    QWidget* createMonitorPage();          // 监控页（设备列表 + 连接参数 + 波形 + 日志）
    QWidget* createStatusHeader();         // 顶部常驻状态条（设备/报警/工单/用户）
    QWidget* createNavigationRail();       // 左侧工业风导航栏
    void syncNavigation();                 // 同步导航按钮的文字/选中/可见性
    void updatePageHeader(int index);      // 刷新页面标题与说明，并做淡入过渡
    void refreshHeader();                  // 刷新顶部状态条与报警横幅
    void refreshFaceplate();               // 刷新设备 Faceplate（名称/地址/状态）
    void appendLog(const QString& message); // 追加带时间戳的日志
    void showQuickStart();                 // 打开"快速上手"说明
    void retranslateUi();                   // 语言切换后重设主框架文案（SY-06）
    void updateStatsLabel();                // 刷新统计文本（每秒一次）
    void refreshLinkHealth();               // 刷新链路健康面板（每秒一次）
    void refreshDeviceList();               // 重建设备列表
    void updateDeviceItem(const QString& id);                 // 更新列表项状态
    void populateLinkCombo(const QString& id);                // 设备 -> 链路下拉
    void applySpecToControls(const QString& id);              // 选中设备/链路 -> 表单
    bool anyLinkConnected(const QString& id) const;           // 是否有任一条链路已连接
    void applyRolePermissions();           // 按当前用户角色启用/禁用操作
    void updateAlarmTab();                 // 报警页签标题显示未恢复数量
    service::DeviceService* currentDevice() const;           // 当前设备的主链路服务对象
    service::DeviceService* currentLink() const;             // 当前选中链路服务对象
    void applyChannelLimits(service::DeviceService* device); // 波形叠加通道阈值
    void resetCapture();                   // 清空波形捕获缓冲
    bool saveCaptureCsv();                 // 把捕获缓冲写成 CSV

    security::AuthService* auth_ = nullptr;             // 当前登录用户（不持有生命周期）
    storage::SystemRepository* systemRepository_ = nullptr; // 审计/报警存储（不持有生命周期）
    service::AlarmMonitor* alarmMonitor_ = nullptr;     // 阈值报警判定
    service::MesUploader* mesUploader_ = nullptr;       // 测试结果上传 MES
    service::UpdateChecker* updateChecker_ = nullptr;   // 轻量版本检测（SY-05）
    bool manualUpdateCheck_ = false;                    // 当前是否为手动检查更新
    core::StationConfig stationConfig_;                 // 工位/线体/治具/SN 规则/MES
    core::LanguageManager languageManager_;             // 运行期语言切换（SY-06）
    // 页签按固定顺序创建；前三段（看板/监控/测试）与后续页签的文案随语言切换刷新。
    enum TabIndex {
        TabDashboard = 0,
        TabMonitor,
        TabTest,
        TabHistory,
        TabTrend,
        TabSpc,
        TabShift,
        TabPlugin,
    };
    QHash<QString, int> alarmCounts_;                   // 每台设备未恢复报警数（列表⚠）
    int auditTabIndex_ = -1;                            // 审计页签下标（仅管理员可见）
    int userTabIndex_ = -1;                             // 用户管理页签（仅管理员可见）
    int alarmTabIndex_ = -1;                            // 报警页签下标

    service::DeviceManager* manager_ = nullptr; // 多设备管理器
    QString currentDeviceId_;                   // 当前选中设备
    QString currentLinkId_;                     // 当前选中链路（空=主链路）
    storage::TestRepository* repository_ = nullptr; // SQLite 仓储
    storage::SummaryWriter* summaryWriter_ = nullptr; // 摘要异步写入

    QTabWidget* tabs_ = nullptr;
    DashboardPage* dashboardPage_ = nullptr;
    TestPage* testPage_ = nullptr;
    HistoryPage* historyPage_ = nullptr;
    TrendPage* trendPage_ = nullptr;
    AlarmPage* alarmPage_ = nullptr;
    AuditPage* auditPage_ = nullptr;
    SpcPage* spcPage_ = nullptr;
    ShiftPage* shiftPage_ = nullptr;
    PluginPage* pluginPage_ = nullptr;
    UserPage* userPage_ = nullptr;

    // ---- 左侧控制区 ----
    QLabel* userLabel_ = nullptr;         // 当前用户/角色
    QLabel* deviceListLabel_ = nullptr;   // "设备列表"标题（随语言切换）
    QLabel* linkLabel_ = nullptr;         // "链路"标题（随语言切换）
    QPushButton* switchUserButton_ = nullptr; // 切换用户
    QPushButton* helpButton_ = nullptr;       // 快速上手（首次启动也会自动弹出）
    QListWidget* deviceList_ = nullptr;  // 设备列表（多设备并发）
    QComboBox* linkCombo_ = nullptr;     // 链路选择（EQ-06 多链路）
    QComboBox* languageCombo_ = nullptr; // 界面语言（SY-06）
    QComboBox* protocolCombo_ = nullptr; // 协议选择：自定义 / Modbus TCP / Modbus RTU / OPC UA / SCPI
    QLineEdit* hostEdit_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    QSpinBox* unitSpin_ = nullptr;       // Modbus 从站地址
    QComboBox* baudCombo_ = nullptr;     // RTU 波特率
    QLineEdit* scpiQueryEdit_ = nullptr;    // SCPI 周期查询命令
    QSpinBox* scpiPollSpin_ = nullptr;      // SCPI 轮询周期（ms）
    QDoubleSpinBox* scpiScaleSpin_ = nullptr; // SCPI 读数换算系数
    QPushButton* connectButton_ = nullptr;
    QPushButton* captureButton_ = nullptr; // 捕获原始波形
    QPushButton* checkButton_ = nullptr;   // 日常点检
    QCheckBox* hexCheck_ = nullptr;        // 报文诊断（HEX 收发日志）
    bool hexLogging_ = false;
    QLabel* captureLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QTableWidget* linkHealthTable_ = nullptr; // 链路健康面板（EQ-06 收尾）

    // ---- 顶部常驻状态条（借鉴 WinCC/Ignition 的全局状态与报警横幅）----
    QLabel* onlineChip_ = nullptr;   // 链路在线数
    QLabel* orderChip_ = nullptr;    // 当前工单
    QLabel* userChip_ = nullptr;     // 当前用户/角色
    QPushButton* alarmChip_ = nullptr;   // 未恢复报警（点击跳到报警页）
    QPushButton* alarmBanner_ = nullptr; // 报警横幅（有报警时显示）

    // ---- 工业风导航与页面标题 ----
    QVector<QPushButton*> navButtons_;   // 与页签一一对应
    QWidget* pageHeader_ = nullptr;
    QLabel* pageTitle_ = nullptr;
    QLabel* pageSubtitle_ = nullptr;

    // ---- 设备 Faceplate（波形上方的身份与状态卡片）----
    QLabel* faceplateName_ = nullptr;
    QLabel* faceplateMeta_ = nullptr;
    QLabel* faceplateState_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;

    // ---- 右侧波形区 ----
    WaveformView* waveform_ = nullptr;

    // ---- 刷新与统计 ----
    QTimer* uiTimer_ = nullptr;
    std::uint64_t pointsLastSecond_ = 0; // 上一秒的采样点计数
    std::uint64_t pointsAdded_ = 0;      // 累计收到的采样点数

    // ---- 原始波形捕获（最近 20 秒，导出 CSV）----
    bool captureActive_ = false;
    bool captureReady_ = false;          // 已有可保存的数据
    int captureChannelCount_ = 0;
    QString captureDeviceName_;
    QStringList captureChannelNames_;    // 捕获开始时的通道名（含单位）
    std::vector<double> captureTimes_;
    std::vector<std::int16_t> captureValues_;
};

} // namespace aegis::ui
