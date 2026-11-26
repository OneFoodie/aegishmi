#pragma once // 头文件保护

#include "test/flow_types.h"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace aegis::ui {

// =============================================================================
// FlowEditorDialog —— 图形化测试流程编辑器
//
// 定位：把原来"手写 JSON"的流程配置变成可编辑的表格 + 属性面板。
//   - 左侧：步骤列表（增删/上移/下移）
//   - 右侧：当前步骤的属性（动作、参数、判定条件、超时/重试）
//   - 顶部：流程级信息（id/名称/产品/默认超时/失败策略）
//
// 保存时用 FlowWriter 序列化回与原文件相同的 JSON 结构，
// 因此保存后测试引擎可以原样执行（见 tst_flow 的 roundtrip 用例）。
// =============================================================================
class FlowEditorDialog : public QDialog {
    Q_OBJECT

public:
    FlowEditorDialog(const test::TestFlow& flow, const QString& path,
                     QWidget* parent = nullptr);

    // 保存成功后的文件路径（可能因"另存为"而变化）。
    QString savedPath() const { return savedPath_; }

private slots:
    void addStep();
    void removeStep();
    void moveStepUp();
    void moveStepDown();
    void onCurrentRowChanged();
    void validateFlow();
    void save();
    void saveAs();

private:
    void buildUi();
    void refreshTable();
    void refreshProperties();
    void updateFieldEnablement(); // 按动作类型启用/禁用属性控件
    void commit();              // 属性面板 → flow_
    void commitFlowFields();    // 流程级字段 → flow_
    bool saveToPath(const QString& path);
    QString nextStepId() const;

    test::TestFlow flow_;
    QString path_;
    QString savedPath_;
    int currentRow_ = -1;
    bool loading_ = false;      // 刷新控件期间屏蔽信号，避免把界面值误写回

    // ---- 流程级字段 ----
    QLineEdit* idEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QLineEdit* productEdit_ = nullptr;
    QLineEdit* flowVersionEdit_ = nullptr; // TP-03：方案版本号
    QSpinBox* defaultTimeoutSpin_ = nullptr;
    QComboBox* onFailureBox_ = nullptr;

    // ---- 步骤列表 ----
    QTableWidget* table_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* upButton_ = nullptr;
    QPushButton* downButton_ = nullptr;

    // ---- 步骤属性 ----
    QLineEdit* stepIdEdit_ = nullptr;
    QLineEdit* stepNameEdit_ = nullptr;
    QComboBox* actionBox_ = nullptr;
    QSpinBox* timeoutSpin_ = nullptr;
    QSpinBox* retriesSpin_ = nullptr;
    QComboBox* stepOnFailBox_ = nullptr;

    QLineEdit* cmdEdit_ = nullptr;
    QLineEdit* linkEdit_ = nullptr;       // EQ-06 多链路：目标链路 id（空 = 主链路）
    QLineEdit* conditionEdit_ = nullptr;  // P1：执行条件（JS 表达式，空 = 总是执行）
    QLineEdit* gotoEdit_ = nullptr;       // P1：执行后跳转的步骤 id（空 = 下一步）
    QSpinBox* channelSpin_ = nullptr;
    QSpinBox* valueMvSpin_ = nullptr;
    QSpinBox* windowMsSpin_ = nullptr;
    QSpinBox* delayMsSpin_ = nullptr;
    QPlainTextEdit* scriptEdit_ = nullptr;
    QLineEdit* flowRefEdit_ = nullptr;      // TP-12a：子流程文件（相对当前方案目录）
    QLineEdit* promptTextEdit_ = nullptr;   // TP-14：操作员确认提示文本
    QSpinBox* promptTimeoutSpin_ = nullptr; // TP-14：确认等待时长（0 = 不限）

    QCheckBox* expectCheck_ = nullptr;    // 是否配置判定条件
    QSpinBox* ackStatusSpin_ = nullptr;
    QComboBox* metricBox_ = nullptr;
    QLineEdit* minEdit_ = nullptr;        // 留空表示不配置下限
    QLineEdit* maxEdit_ = nullptr;        // 留空表示不配置上限
    QLineEdit* unitEdit_ = nullptr;
    QLineEdit* versionEdit_ = nullptr;        // readVersion：期望版本号
    QLineEdit* versionPatternEdit_ = nullptr; // readVersion：期望版本正则
};

} // namespace aegis::ui
