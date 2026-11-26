#include "ui/flow_editor_dialog.h"

#include "test/flow_validator.h"
#include "test/flow_writer.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace aegis::ui {

namespace {

constexpr int kColumnCount = 5; // # / 名称 / 动作 / 超时 / 重试

QString actionLabel(test::StepAction action)
{
    switch (action) {
    case test::StepAction::Command:
        return QStringLiteral("下发命令");
    case test::StepAction::Delay:
        return QStringLiteral("延时");
    case test::StepAction::Measure:
        return QStringLiteral("采集判定");
    case test::StepAction::Script:
        return QStringLiteral("脚本");
    case test::StepAction::ReadVersion:
        return QStringLiteral("读取版本");
    case test::StepAction::SubFlow:
        return QStringLiteral("子流程");
    case test::StepAction::Prompt:
        return QStringLiteral("操作员确认");
    case test::StepAction::Assert:
        return QStringLiteral("断言(未实现)");
    case test::StepAction::Unknown:
        break;
    }
    return QStringLiteral("未知");
}

} // namespace

FlowEditorDialog::FlowEditorDialog(const test::TestFlow& flow, const QString& path,
                                   QWidget* parent)
    : QDialog(parent)
    , flow_(flow)
    , path_(path)
{
    buildUi();
    refreshTable();
    if (!flow_.steps.isEmpty()) {
        loading_ = true;
        table_->setCurrentCell(0, 0);
        loading_ = false;
        currentRow_ = 0;
    }
    refreshProperties();
}

void FlowEditorDialog::buildUi()
{
    setWindowTitle(QStringLiteral("测试流程编辑器"));
    resize(1080, 660);

    auto* root = new QVBoxLayout(this);

    // ---- 流程级字段 ----
    auto* flowGroup = new QGroupBox(QStringLiteral("流程"), this);
    auto* flowRow = new QHBoxLayout(flowGroup);

    idEdit_ = new QLineEdit(flowGroup);
    idEdit_->setPlaceholderText(QStringLiteral("流程唯一标识，如 FLOW-LED-001"));
    nameEdit_ = new QLineEdit(flowGroup);
    productEdit_ = new QLineEdit(flowGroup);
    flowVersionEdit_ = new QLineEdit(flowGroup);
    flowVersionEdit_->setPlaceholderText(QStringLiteral("如 1.2.0；保存时写入校验和"));
    defaultTimeoutSpin_ = new QSpinBox(flowGroup);
    defaultTimeoutSpin_->setRange(10, 60000);
    defaultTimeoutSpin_->setSuffix(QStringLiteral(" ms"));
    onFailureBox_ = new QComboBox(flowGroup);
    onFailureBox_->addItem(QStringLiteral("失败即终止"), QStringLiteral("abort"));
    onFailureBox_->addItem(QStringLiteral("失败后继续"), QStringLiteral("continue"));

    auto addField = [flowRow, flowGroup](const QString& label, QWidget* widget) {
        flowRow->addWidget(new QLabel(label, flowGroup));
        flowRow->addWidget(widget);
    };
    addField(QStringLiteral("ID"), idEdit_);
    addField(QStringLiteral("名称"), nameEdit_);
    addField(QStringLiteral("产品"), productEdit_);
    addField(QStringLiteral("方案版本"), flowVersionEdit_);
    addField(QStringLiteral("默认超时"), defaultTimeoutSpin_);
    addField(QStringLiteral("失败策略"), onFailureBox_);
    root->addWidget(flowGroup);

    auto* body = new QHBoxLayout();

    // ---- 左：步骤列表 + 操作按钮 ----
    auto* left = new QVBoxLayout();
    table_ = new QTableWidget(0, kColumnCount, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("#"),
                                       QStringLiteral("名称"),
                                       QStringLiteral("动作"),
                                       QStringLiteral("超时"),
                                       QStringLiteral("重试")});
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int column : {0, 2, 3, 4})
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    left->addWidget(table_, 1);

    auto* stepButtons = new QHBoxLayout();
    addButton_ = new QPushButton(QStringLiteral("添加步骤"), this);
    removeButton_ = new QPushButton(QStringLiteral("删除步骤"), this);
    upButton_ = new QPushButton(QStringLiteral("上移"), this);
    downButton_ = new QPushButton(QStringLiteral("下移"), this);
    stepButtons->addWidget(addButton_);
    stepButtons->addWidget(removeButton_);
    stepButtons->addWidget(upButton_);
    stepButtons->addWidget(downButton_);
    left->addLayout(stepButtons);

    // ---- 右：步骤属性 ----
    auto* propsGroup = new QGroupBox(QStringLiteral("步骤属性"), this);
    auto* form = new QFormLayout(propsGroup);

    stepIdEdit_ = new QLineEdit(propsGroup);
    stepNameEdit_ = new QLineEdit(propsGroup);
    actionBox_ = new QComboBox(propsGroup);
    actionBox_->addItem(QStringLiteral("下发命令"), int(test::StepAction::Command));
    actionBox_->addItem(QStringLiteral("延时"), int(test::StepAction::Delay));
    actionBox_->addItem(QStringLiteral("采集判定"), int(test::StepAction::Measure));
    actionBox_->addItem(QStringLiteral("脚本"), int(test::StepAction::Script));
    actionBox_->addItem(QStringLiteral("读取版本"), int(test::StepAction::ReadVersion));
    actionBox_->addItem(QStringLiteral("子流程"), int(test::StepAction::SubFlow));
    actionBox_->addItem(QStringLiteral("操作员确认"), int(test::StepAction::Prompt));

    timeoutSpin_ = new QSpinBox(propsGroup);
    timeoutSpin_->setRange(-1, 60000);
    timeoutSpin_->setSpecialValueText(QStringLiteral("使用默认"));
    timeoutSpin_->setSuffix(QStringLiteral(" ms"));
    retriesSpin_ = new QSpinBox(propsGroup);
    retriesSpin_->setRange(0, 10);
    stepOnFailBox_ = new QComboBox(propsGroup);
    stepOnFailBox_->addItem(QStringLiteral("继承流程"), QString());
    stepOnFailBox_->addItem(QStringLiteral("失败即终止"), QStringLiteral("abort"));
    stepOnFailBox_->addItem(QStringLiteral("失败后继续"), QStringLiteral("continue"));

    form->addRow(QStringLiteral("步骤 ID"), stepIdEdit_);
    form->addRow(QStringLiteral("步骤名称"), stepNameEdit_);
    form->addRow(QStringLiteral("动作"), actionBox_);
    form->addRow(QStringLiteral("超时"), timeoutSpin_);
    form->addRow(QStringLiteral("重试次数"), retriesSpin_);
    form->addRow(QStringLiteral("失败处理"), stepOnFailBox_);

    cmdEdit_ = new QLineEdit(propsGroup);
    cmdEdit_->setPlaceholderText(QStringLiteral("SET_VOUT / RESET …"));
    linkEdit_ = new QLineEdit(propsGroup);
    linkEdit_->setPlaceholderText(QStringLiteral("留空 = 主链路；如 aux"));
    conditionEdit_ = new QLineEdit(propsGroup);
    conditionEdit_->setPlaceholderText(
        QStringLiteral("JS 表达式，如 i < 3；留空总是执行"));
    gotoEdit_ = new QLineEdit(propsGroup);
    gotoEdit_->setPlaceholderText(QStringLiteral("执行后跳转的步骤 ID；留空继续下一步"));
    channelSpin_ = new QSpinBox(propsGroup);
    channelSpin_->setRange(-1, 31);
    valueMvSpin_ = new QSpinBox(propsGroup);
    valueMvSpin_->setRange(-30000, 30000);
    windowMsSpin_ = new QSpinBox(propsGroup);
    windowMsSpin_->setRange(10, 10000);
    windowMsSpin_->setSuffix(QStringLiteral(" ms"));
    delayMsSpin_ = new QSpinBox(propsGroup);
    delayMsSpin_->setRange(1, 60000);
    delayMsSpin_->setSuffix(QStringLiteral(" ms"));
    scriptEdit_ = new QPlainTextEdit(propsGroup);
    scriptEdit_->setPlaceholderText(
        QStringLiteral("JavaScript：可用 setVout(ch, mv)、measureAvg(ch, ms) 等"));
    scriptEdit_->setMinimumHeight(90);
    flowRefEdit_ = new QLineEdit(propsGroup);
    flowRefEdit_->setPlaceholderText(QStringLiteral("子流程文件，如 common/precheck.json"));
    promptTextEdit_ = new QLineEdit(propsGroup);
    promptTextEdit_->setPlaceholderText(QStringLiteral("提示操作员确认的内容"));
    promptTimeoutSpin_ = new QSpinBox(propsGroup);
    promptTimeoutSpin_->setRange(0, 600000);
    promptTimeoutSpin_->setSuffix(QStringLiteral(" ms"));
    promptTimeoutSpin_->setSpecialValueText(QStringLiteral("不限时"));

    // 字段说明：编辑器信息密度高，悬停提示能减少"这个参数到底填什么"的困惑。
    stepIdEdit_->setToolTip(QStringLiteral("步骤唯一标识，用于 goto 跳转与结果定位"));
    stepNameEdit_->setToolTip(QStringLiteral("界面上显示的名称，建议写清测试项"));
    actionBox_->setToolTip(QStringLiteral(
        "command 命令 / delay 等待 / measure 采集 / script 脚本 / readVersion 版本 / "
        "plugin 插件 / subFlow 子流程 / prompt 操作员确认"));
    timeoutSpin_->setToolTip(QStringLiteral("单步超时；-1 表示使用流程默认超时"));
    retriesSpin_->setToolTip(QStringLiteral("失败后自动重试次数（0~10）"));
    stepOnFailBox_->setToolTip(QStringLiteral("本步失败后：终止整个流程 / 继续后续步骤"));
    cmdEdit_->setToolTip(QStringLiteral("命令名：SET_VOUT、SET_IOUT、READ_MEASURE、RESET；读取版本可填 *IDN?"));
    linkEdit_->setToolTip(QStringLiteral("目标链路 id；留空使用主链路（多链路设备才需要填）"));
    channelSpin_->setToolTip(QStringLiteral("通道号；-1 表示不指定"));
    valueMvSpin_->setToolTip(QStringLiteral("命令数值参数（mV 或 0.01A，由命令决定）"));
    windowMsSpin_->setToolTip(QStringLiteral("measure 的采样窗口长度，越长越稳定、耗时越久"));
    delayMsSpin_->setToolTip(QStringLiteral("delay 步骤的等待时间"));
    scriptEdit_->setToolTip(QStringLiteral(
        "JavaScript：setVout/setIout/measureAvg/measureOnce/sleep/log/getVar/setVar"));
    flowRefEdit_->setToolTip(QStringLiteral(
        "子流程文件，相对当前方案目录；禁止循环引用"));
    promptTextEdit_->setToolTip(QStringLiteral("运行时弹窗给操作员看的提示内容"));
    promptTimeoutSpin_->setToolTip(QStringLiteral("等待操作员确认的时长；0 表示不限时"));
    conditionEdit_->setToolTip(QStringLiteral(
        "JS 条件表达式，false 时跳过本步；可用变量与 lastValue/lastOk"));
    gotoEdit_->setToolTip(QStringLiteral(
        "本步执行后跳转到指定步骤 id；配合条件可实现循环（有 5000 步保护）"));
    expectCheck_->setToolTip(QStringLiteral("勾选后本步才有判定；不勾选只执行不判定"));
    ackStatusSpin_->setToolTip(QStringLiteral("command 期望的应答状态码，0 表示成功"));
    metricBox_->setToolTip(QStringLiteral("measure 使用的统计量"));
    minEdit_->setToolTip(QStringLiteral("统计量下限；留空不判下限"));
    maxEdit_->setToolTip(QStringLiteral("统计量上限；留空不判上限"));
    unitEdit_->setToolTip(QStringLiteral("仅用于报告显示，不参与判定"));
    versionEdit_->setToolTip(QStringLiteral("期望版本号（去空格后精确比对）"));
    versionPatternEdit_->setToolTip(QStringLiteral("期望版本正则，与“期望版本”二选一"));

    form->addRow(QStringLiteral("命令"), cmdEdit_);
    form->addRow(QStringLiteral("链路"), linkEdit_);
    form->addRow(QStringLiteral("通道"), channelSpin_);
    form->addRow(QStringLiteral("数值(mV)"), valueMvSpin_);
    form->addRow(QStringLiteral("采样窗口"), windowMsSpin_);
    form->addRow(QStringLiteral("延时"), delayMsSpin_);
    form->addRow(QStringLiteral("脚本"), scriptEdit_);
    form->addRow(QStringLiteral("子流程文件"), flowRefEdit_);
    form->addRow(QStringLiteral("提示文本"), promptTextEdit_);
    form->addRow(QStringLiteral("确认等待"), promptTimeoutSpin_);
    form->addRow(QStringLiteral("执行条件"), conditionEdit_);
    form->addRow(QStringLiteral("跳转(goto)"), gotoEdit_);

    expectCheck_ = new QCheckBox(QStringLiteral("启用判定条件"), propsGroup);
    ackStatusSpin_ = new QSpinBox(propsGroup);
    ackStatusSpin_->setRange(0, 255);
    metricBox_ = new QComboBox(propsGroup);
    metricBox_->addItem(QStringLiteral("平均值 avg"), int(test::Metric::Avg));
    metricBox_->addItem(QStringLiteral("最小值 min"), int(test::Metric::Min));
    metricBox_->addItem(QStringLiteral("最大值 max"), int(test::Metric::Max));
    metricBox_->addItem(QStringLiteral("有效值 rms"), int(test::Metric::Rms));
    metricBox_->addItem(QStringLiteral("峰峰值 p2p"), int(test::Metric::PeakToPeak));
    minEdit_ = new QLineEdit(propsGroup);
    minEdit_->setPlaceholderText(QStringLiteral("留空表示不设下限"));
    maxEdit_ = new QLineEdit(propsGroup);
    maxEdit_->setPlaceholderText(QStringLiteral("留空表示不设上限"));
    unitEdit_ = new QLineEdit(propsGroup);
    unitEdit_->setPlaceholderText(QStringLiteral("仅用于显示，如 mV"));
    versionEdit_ = new QLineEdit(propsGroup);
    versionEdit_->setPlaceholderText(QStringLiteral("例如 AegisSim,DMM,0001,1.0"));
    versionPatternEdit_ = new QLineEdit(propsGroup);
    versionPatternEdit_->setPlaceholderText(QStringLiteral("例如 .*,1\\.0$（与期望版本二选一）"));

    form->addRow(QString(), expectCheck_);
    form->addRow(QStringLiteral("应答状态码"), ackStatusSpin_);
    form->addRow(QStringLiteral("统计指标"), metricBox_);
    form->addRow(QStringLiteral("下限"), minEdit_);
    form->addRow(QStringLiteral("上限"), maxEdit_);
    form->addRow(QStringLiteral("单位"), unitEdit_);
    form->addRow(QStringLiteral("期望版本"), versionEdit_);
    form->addRow(QStringLiteral("版本正则"), versionPatternEdit_);

    body->addLayout(left, 3);
    body->addWidget(propsGroup, 2);
    root->addLayout(body, 1);

    // ---- 底部按钮 ----
    auto* bottom = new QHBoxLayout();
    auto* validateButton = new QPushButton(QStringLiteral("校验"), this);
    auto* saveButton = new QPushButton(QStringLiteral("保存"), this);
    auto* saveAsButton = new QPushButton(QStringLiteral("另存为…"), this);
    auto* cancelButton = new QPushButton(QStringLiteral("取消"), this);
    validateButton->setToolTip(QStringLiteral("检查必填项、链路/子流程引用与判定条件，不写文件"));
    saveButton->setToolTip(QStringLiteral("校验通过后写回当前方案文件（含 SHA-256 校验和）"));
    saveAsButton->setToolTip(QStringLiteral("另存为新方案文件，不覆盖原文件"));
    saveButton->setDefault(true);
    bottom->addWidget(validateButton);
    bottom->addStretch(1);
    bottom->addWidget(saveButton);
    bottom->addWidget(saveAsButton);
    bottom->addWidget(cancelButton);
    root->addLayout(bottom);

    connect(table_, &QTableWidget::currentCellChanged, this,
            [this](int, int, int, int) { onCurrentRowChanged(); });
    connect(addButton_, &QPushButton::clicked, this, &FlowEditorDialog::addStep);
    connect(removeButton_, &QPushButton::clicked, this, &FlowEditorDialog::removeStep);
    connect(upButton_, &QPushButton::clicked, this, &FlowEditorDialog::moveStepUp);
    connect(downButton_, &QPushButton::clicked, this, &FlowEditorDialog::moveStepDown);
    connect(validateButton, &QPushButton::clicked, this, &FlowEditorDialog::validateFlow);
    connect(saveButton, &QPushButton::clicked, this, &FlowEditorDialog::save);
    connect(saveAsButton, &QPushButton::clicked, this, &FlowEditorDialog::saveAs);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);

    // 动作/判定变化时立刻刷新可用性，避免填了无关字段。
    connect(actionBox_, &QComboBox::currentIndexChanged, this,
            [this](int) { updateFieldEnablement(); });
    connect(expectCheck_, &QCheckBox::toggled, this,
            [this](bool) { updateFieldEnablement(); });
}

void FlowEditorDialog::refreshTable()
{
    loading_ = true;
    table_->setRowCount(flow_.steps.size());

    for (int row = 0; row < flow_.steps.size(); ++row) {
        const test::TestStep& step = flow_.steps.at(row);
        const QString texts[kColumnCount] = {
            QString::number(row + 1),
            step.name,
            actionLabel(step.action),
            step.timeoutMs < 0 ? QStringLiteral("默认") : QString::number(step.timeoutMs),
            QString::number(step.retries),
        };

        for (int column = 0; column < kColumnCount; ++column) {
            auto* item = new QTableWidgetItem(texts[column]);
            if (column != 1)
                item->setTextAlignment(Qt::AlignCenter);
            table_->setItem(row, column, item);
        }
    }
    loading_ = false;
}

void FlowEditorDialog::refreshProperties()
{
    loading_ = true;

    idEdit_->setText(flow_.id);
    nameEdit_->setText(flow_.name);
    productEdit_->setText(flow_.product);
    flowVersionEdit_->setText(flow_.version);
    defaultTimeoutSpin_->setValue(flow_.defaultTimeoutMs);
    onFailureBox_->setCurrentIndex(
        onFailureBox_->findData(flow_.onFailure.isEmpty() ? QStringLiteral("abort")
                                                         : flow_.onFailure));

    const bool hasStep = currentRow_ >= 0 && currentRow_ < flow_.steps.size();
    if (hasStep) {
        const test::TestStep& step = flow_.steps.at(currentRow_);

        stepIdEdit_->setText(step.id);
        stepNameEdit_->setText(step.name);
        actionBox_->setCurrentIndex(actionBox_->findData(int(step.action)));
        timeoutSpin_->setValue(step.timeoutMs);
        retriesSpin_->setValue(step.retries);
        stepOnFailBox_->setCurrentIndex(stepOnFailBox_->findData(step.onFail));

        cmdEdit_->setText(step.cmd);
        linkEdit_->setText(step.link);
        conditionEdit_->setText(step.condition);
        gotoEdit_->setText(step.gotoStep);
        channelSpin_->setValue(step.channel < 0 ? 0 : step.channel);
        valueMvSpin_->setValue(step.valueMv);
        windowMsSpin_->setValue(step.windowMs > 0 ? step.windowMs : 500);
        delayMsSpin_->setValue(step.delayMs > 0 ? step.delayMs : 100);
        scriptEdit_->setPlainText(step.script);
        flowRefEdit_->setText(step.flowRef);
        promptTextEdit_->setText(step.promptText);
        promptTimeoutSpin_->setValue(step.promptTimeoutMs);

        expectCheck_->setChecked(step.expect.present);
        ackStatusSpin_->setValue(step.expect.ackStatus);
        metricBox_->setCurrentIndex(
            step.expect.metric == test::Metric::Unknown
                ? 0
                : metricBox_->findData(int(step.expect.metric)));
        minEdit_->setText(step.expect.hasMin ? QString::number(step.expect.min) : QString());
        maxEdit_->setText(step.expect.hasMax ? QString::number(step.expect.max) : QString());
        unitEdit_->setText(step.expect.unit);
        versionEdit_->setText(step.expect.version);
        versionPatternEdit_->setText(step.expect.versionPattern);
    }

    stepIdEdit_->setEnabled(hasStep);
    stepNameEdit_->setEnabled(hasStep);
    actionBox_->setEnabled(hasStep);
    timeoutSpin_->setEnabled(hasStep);
    retriesSpin_->setEnabled(hasStep);
    stepOnFailBox_->setEnabled(hasStep);
    removeButton_->setEnabled(hasStep);
    upButton_->setEnabled(hasStep && currentRow_ > 0);
    downButton_->setEnabled(hasStep && currentRow_ + 1 < flow_.steps.size());

    loading_ = false;
    updateFieldEnablement();
}

void FlowEditorDialog::updateFieldEnablement()
{
    const bool hasStep = currentRow_ >= 0 && currentRow_ < flow_.steps.size();
    const auto action = static_cast<test::StepAction>(actionBox_->currentData().toInt());

    cmdEdit_->setEnabled(hasStep
                         && (action == test::StepAction::Command
                             || action == test::StepAction::ReadVersion));
    // 链路只对"下发命令/采集/读版本"有意义；留空即主链路。
    linkEdit_->setEnabled(hasStep
                          && (action == test::StepAction::Command
                              || action == test::StepAction::Measure
                              || action == test::StepAction::ReadVersion));
    channelSpin_->setEnabled(hasStep
                             && (action == test::StepAction::Command
                                 || action == test::StepAction::Measure));
    valueMvSpin_->setEnabled(hasStep && action == test::StepAction::Command);
    windowMsSpin_->setEnabled(hasStep && action == test::StepAction::Measure);
    delayMsSpin_->setEnabled(hasStep && action == test::StepAction::Delay);
    scriptEdit_->setEnabled(hasStep && action == test::StepAction::Script);
    flowRefEdit_->setEnabled(hasStep && action == test::StepAction::SubFlow);
    promptTextEdit_->setEnabled(hasStep && action == test::StepAction::Prompt);
    promptTimeoutSpin_->setEnabled(hasStep && action == test::StepAction::Prompt);
    // 流程控制对所有动作都适用（循环计数通常由 script 步骤完成）。
    conditionEdit_->setEnabled(hasStep);
    gotoEdit_->setEnabled(hasStep);

    // 只有 command / measure / readVersion 支持 expect（delay、script 配置了会被校验器拒绝）。
    const bool expectAllowed = hasStep
        && (action == test::StepAction::Command || action == test::StepAction::Measure
            || action == test::StepAction::ReadVersion);
    expectCheck_->setEnabled(expectAllowed);

    const bool expectOn = expectAllowed && expectCheck_->isChecked();
    const bool isMeasure = action == test::StepAction::Measure;
    const bool isVersion = action == test::StepAction::ReadVersion;
    ackStatusSpin_->setEnabled(expectOn && action == test::StepAction::Command);
    metricBox_->setEnabled(expectOn && isMeasure);
    minEdit_->setEnabled(expectOn && isMeasure);
    maxEdit_->setEnabled(expectOn && isMeasure);
    unitEdit_->setEnabled(expectOn && isMeasure);
    versionEdit_->setEnabled(expectOn && isVersion);
    versionPatternEdit_->setEnabled(expectOn && isVersion);
}

void FlowEditorDialog::commitFlowFields()
{
    flow_.id = idEdit_->text().trimmed();
    flow_.name = nameEdit_->text().trimmed();
    flow_.product = productEdit_->text().trimmed();
    flow_.version = flowVersionEdit_->text().trimmed();
    flow_.defaultTimeoutMs = defaultTimeoutSpin_->value();
    flow_.onFailure = onFailureBox_->currentData().toString();
}

void FlowEditorDialog::commit()
{
    if (loading_)
        return;

    commitFlowFields();
    if (currentRow_ < 0 || currentRow_ >= flow_.steps.size())
        return;

    test::TestStep& step = flow_.steps[currentRow_];
    step.id = stepIdEdit_->text().trimmed();
    step.name = stepNameEdit_->text().trimmed();
    step.action = static_cast<test::StepAction>(actionBox_->currentData().toInt());
    step.timeoutMs = timeoutSpin_->value();
    step.retries = retriesSpin_->value();
    step.onFail = stepOnFailBox_->currentData().toString();

    step.cmd = cmdEdit_->text().trimmed();
    step.link = linkEdit_->text().trimmed();
    step.condition = conditionEdit_->text().trimmed();
    step.gotoStep = gotoEdit_->text().trimmed();
    step.channel = channelSpin_->value();
    step.valueMv = valueMvSpin_->value();
    step.windowMs = windowMsSpin_->value();
    step.delayMs = delayMsSpin_->value();
    step.script = scriptEdit_->toPlainText();
    step.flowRef = flowRefEdit_->text().trimmed();
    step.promptText = promptTextEdit_->text().trimmed();
    step.promptTimeoutMs = promptTimeoutSpin_->value();

    step.expect.present = expectCheck_->isChecked();
    step.expect.ackStatus = ackStatusSpin_->value();
    step.expect.metric = static_cast<test::Metric>(metricBox_->currentData().toInt());

    const QString minText = minEdit_->text().trimmed();
    step.expect.hasMin = !minText.isEmpty();
    if (step.expect.hasMin) {
        bool ok = false;
        const double value = minText.toDouble(&ok);
        if (ok)
            step.expect.min = value;
    }

    const QString maxText = maxEdit_->text().trimmed();
    step.expect.hasMax = !maxText.isEmpty();
    if (step.expect.hasMax) {
        bool ok = false;
        const double value = maxText.toDouble(&ok);
        if (ok)
            step.expect.max = value;
    }

    step.expect.unit = unitEdit_->text().trimmed();
    step.expect.version = versionEdit_->text().trimmed();
    step.expect.versionPattern = versionPatternEdit_->text().trimmed();
}

void FlowEditorDialog::onCurrentRowChanged()
{
    if (loading_)
        return;

    const int row = table_->currentRow();
    if (row == currentRow_)
        return;

    commit(); // 先把上一行的编辑写回
    currentRow_ = row;
    refreshTable(); // 表格文本反映刚写回的值（保持当前选中行）
    refreshProperties();
}

QString FlowEditorDialog::nextStepId() const
{
    for (int n = flow_.steps.size() + 1;; ++n) {
        const QString candidate = QStringLiteral("step%1").arg(n);
        bool used = false;
        for (const test::TestStep& step : flow_.steps) {
            if (step.id == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
    }
}

void FlowEditorDialog::addStep()
{
    commit();

    test::TestStep step;
    step.id = nextStepId();
    step.name = QStringLiteral("新步骤");
    step.action = test::StepAction::Command;
    step.timeoutMs = -1;
    step.channel = -1;
    flow_.steps.append(step);

    loading_ = true;
    currentRow_ = flow_.steps.size() - 1;
    refreshTable();
    table_->setCurrentCell(currentRow_, 0);
    loading_ = false;

    refreshProperties();
}

void FlowEditorDialog::removeStep()
{
    if (currentRow_ < 0 || currentRow_ >= flow_.steps.size())
        return;

    loading_ = true;
    flow_.steps.removeAt(currentRow_);
    currentRow_ = std::min(currentRow_, int(flow_.steps.size()) - 1);
    refreshTable();
    if (currentRow_ >= 0)
        table_->setCurrentCell(currentRow_, 0);
    loading_ = false;

    refreshProperties();
}

void FlowEditorDialog::moveStepUp()
{
    if (currentRow_ <= 0 || currentRow_ >= flow_.steps.size())
        return;

    commit();
    flow_.steps.swapItemsAt(currentRow_, currentRow_ - 1);
    --currentRow_;

    loading_ = true;
    refreshTable();
    table_->setCurrentCell(currentRow_, 0);
    loading_ = false;

    refreshProperties();
}

void FlowEditorDialog::moveStepDown()
{
    if (currentRow_ < 0 || currentRow_ + 1 >= flow_.steps.size())
        return;

    commit();
    flow_.steps.swapItemsAt(currentRow_, currentRow_ + 1);
    ++currentRow_;

    loading_ = true;
    refreshTable();
    table_->setCurrentCell(currentRow_, 0);
    loading_ = false;

    refreshProperties();
}

void FlowEditorDialog::validateFlow()
{
    commit();

    const QVector<test::FlowIssue> issues = test::FlowValidator::validate(flow_);
    if (issues.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("校验"),
                                 QStringLiteral("校验通过：共 %1 个步骤。")
                                     .arg(flow_.steps.size()));
        return;
    }

    QStringList lines;
    for (const test::FlowIssue& issue : issues)
        lines << QStringLiteral("%1：%2").arg(issue.fieldPath, issue.message);
    QMessageBox::warning(this, QStringLiteral("校验未通过"),
                         lines.join(QLatin1Char('\n')));
}

bool FlowEditorDialog::saveToPath(const QString& path)
{
    QString error;
    if (!test::FlowWriter::saveToFile(flow_, path, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存失败"),
                             QStringLiteral("%1\n%2").arg(path, error));
        return false;
    }

    path_ = path;
    savedPath_ = path;
    accept();
    return true;
}

void FlowEditorDialog::save()
{
    commit();
    if (path_.isEmpty()) {
        saveAs();
        return;
    }
    saveToPath(path_);
}

void FlowEditorDialog::saveAs()
{
    commit();

    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("另存为"), path_, QStringLiteral("流程配置 (*.json)"));
    if (path.isEmpty())
        return;

    saveToPath(path);
}

} // namespace aegis::ui
