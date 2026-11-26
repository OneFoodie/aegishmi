#pragma once // 头文件保护

#include "core/station_config.h"
#include "test/flow_types.h"
#include "test/test_device.h" // RealClock
#include "test/test_result.h"

#include <QSet>
#include <QThread>
#include <QWidget>

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace aegis::test {
class TestEngine;
}

namespace aegis::security {
class AuthService;
}

namespace aegis::service {
class BomVerifier;
class DeviceManager;
class DeviceService;
}

namespace aegis::storage {
class TestRepository;
}

namespace aegis::ui {

class DeviceProxy; // ITestDevice 转发代理（见 test_page.cpp）

// =============================================================================
// TestPage —— 自动测试页
//
// 职责：
//   1. 加载并校验流程配置（复用 M3.1 的 FlowLoader / FlowValidator）
//   2. 把流程交给工作线程里的 TestEngine 执行，界面逐行刷新
//   3. 展示进度、结果摘要、失败原因
//
// 多设备（P2）：
//   - 界面通过 setDevice() 切换"当前测试设备"；
//   - 引擎持有的是 DeviceProxy，内部用原子指针转发到当前设备，
//     因此切换设备不需要重建线程/引擎；运行期间界面会禁止切换。
//
// 线程模型：
//   - 引擎跑在 engineThread_ 里，sendCommand/sampleWindow 会阻塞引擎线程
//   - 界面只通过队列信号接收步骤进度，不参与执行
// =============================================================================
class TestPage : public QWidget {
    Q_OBJECT

public:
    explicit TestPage(storage::TestRepository* repository,
                      security::AuthService* auth,
                      const core::StationConfig& station,
                      QWidget* parent = nullptr);
    ~TestPage() override;

    // 切换当前测试设备（分析下发的命令/采集窗口都走这台设备）。
    // 多链路设备按步骤的 linkId 路由，未指定时走主链路。
    void setDevice(service::DeviceManager* manager, const QString& deviceId);

    // 切换当前生产工单（空表示未选择，测试将被拒绝）。
    void setWorkOrder(const QString& orderNo, const QString& product);
    QString workOrderNo() const { return workOrderNo_; }

    // 是否正在执行流程（界面据此禁止切换设备）。
    bool isRunning() const { return running_; }

    // 观察员只读：禁用加载方案/开始/停止。
    void setOperationAllowed(bool allowed);

signals:
    void runningChanged(bool running);
    // 一次运行成功落库（主界面据此触发 MES 上传）。
    void runSaved(int runId);

private slots:
    void loadFlow();                                   // 选择并加载 JSON
    void editFlow();                                   // 图形化编辑当前方案
    void debugStep();                                  // 单步执行（调试）
    void startTest();                                  // 开始执行
    void stopTest();                                   // 请求停止
    void onStepStarted(int index);                     // 某步骤开始
    void onStepFinished(int index, const test::StepResult& result); // 某步骤结束
    void onRunFinished(const test::RunResult& result); // 整体结束
    void onDeviceStateChanged(bool connected, const QString& detail); // 设备连接变化
    void checkMaterial();                              // 扫码料号 → MES BOM 核对
    void unlockMaterial();                             // 班组长解锁工位

private:
    void buildUi();
    void setRunning(bool running);
    void updateButtons();                  // 统一刷新按钮可用状态（角色 + 运行 + 连接）
    void updateWorkOrderInfo();            // 刷新左侧工单显示（含绑定方案）
    bool loadFlowFile(const QString& path, bool interactive); // 加载/校验/接受方案
    bool validateSn(const QString& sn, QString* error) const; // SN 格式校验
    bool checkFixtureReady(QString* error); // 治具寿命/校准检查
    bool checkAndon(QString* error);        // Andon 停线检查
    // MES 过站/工单校验（同步等待，超时按 mesFailOpen 处理）。
    bool checkMesStation(const QString& workOrder, const QString& sn,
                         const QString& product, QString* error);
    bool checkQualification(const QString& product, QString* error) const; // 操作员资质
    bool ensureSopRead(const QString& product); // 电子 SOP 强制阅读
    void evaluateAndon(const QString& verdict); // 落库后评估是否触发停线
    void showFlowError(const QString& message);
    void showFlowInfo();
    void resetTable();
    void updateRow(int index, const test::StepResult& result);

    service::DeviceService* device_ = nullptr; // 当前测试设备（界面显示用）
    DeviceProxy* proxy_ = nullptr;             // 引擎实际持有的转发代理
    storage::TestRepository* repository_ = nullptr; // 运行结束后落库
    security::AuthService* auth_ = nullptr;    // 审计：谁在什么时候跑了什么方案
    core::StationConfig station_;              // 工位/线体/治具/SN 规则

    test::TestFlow flow_;
    bool flowValid_ = false;
    bool running_ = false;
    bool operationAllowed_ = true;             // 观察员为 false

    QString flowPath_;                         // 当前方案文件路径（编辑器保存回原文件）
    bool debugMode_ = false;                   // 当前这次执行是"单步调试"，不落库
    int debugIndex_ = 0;                       // 下一次单步执行的步骤序号

    QString workOrderNo_;
    QString workOrderProduct_;
    QString currentSn_;                        // 本次运行的 SN
    bool currentRetest_ = false;               // 本次是否为复测
    QSet<QString> sopConfirmed_;               // 已确认阅读的 SOP（工单|产品|版本）

    QPushButton* loadButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* stepButton_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QLineEdit* snEdit_ = nullptr;
    QLineEdit* materialEdit_ = nullptr;            // 物料扫码输入
    QPushButton* materialButton_ = nullptr;        // 执行料号核对
    QPushButton* unlockButton_ = nullptr;          // 班组长解锁
    QLabel* materialInfo_ = nullptr;               // 核对结果/锁定提示
    service::BomVerifier* bom_ = nullptr;          // 物料防错（扫码 + BOM 查询）
    QLabel* workOrderInfo_ = nullptr;
    QLabel* flowInfo_ = nullptr;
    QLabel* deviceInfo_ = nullptr;
    QLabel* summary_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTableWidget* table_ = nullptr;

    // 引擎与时钟。时钟必须是普通对象，生命周期覆盖引擎。
    test::RealClock clock_;
    QThread engineThread_;
    test::TestEngine* engine_ = nullptr;
};

} // namespace aegis::ui
