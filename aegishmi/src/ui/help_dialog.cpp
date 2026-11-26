#include "ui/help_dialog.h"

#include "ui/theme.h"

#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

namespace aegis::ui {

namespace {

const QString kDismissedKey = QStringLiteral("ui/quickStartDismissed");

// 一条"步骤"：序号 + 标题 + 说明。
void addStep(QGridLayout* grid, int row, const QString& number,
             const QString& title, const QString& description)
{
    auto* badge = new QLabel(number);
    badge->setFixedSize(26, 26);
    badge->setAlignment(Qt::AlignCenter);
    badge->setStyleSheet(QStringLiteral(
        "background:#37474f;color:#ffffff;border-radius:13px;font-weight:600;"));

    auto* titleLabel = new QLabel(title);
    titleLabel->setStyleSheet(QStringLiteral("font-weight:600;color:#111827;"));
    auto* descLabel = makeHint(description);

    auto* text = new QVBoxLayout();
    text->setSpacing(2);
    text->addWidget(titleLabel);
    text->addWidget(descLabel);

    grid->addWidget(badge, row, 0, Qt::AlignTop);
    grid->addLayout(text, row, 1);
}

// 页签地图的一行：页签名 + 一句话说明。
void addTabRow(QGridLayout* grid, int row, const QString& name, const QString& description)
{
    auto* nameLabel = new QLabel(name);
    nameLabel->setStyleSheet(QStringLiteral("font-weight:600;color:#263238;"));
    grid->addWidget(nameLabel, row, 0);
    grid->addWidget(makeHint(description), row, 1);
}

QFrame* makeCard(const QString& title)
{
    auto* card = new QFrame();
    card->setProperty("role", QStringLiteral("card"));

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto* titleLabel = new QLabel(title);
    titleLabel->setStyleSheet(QStringLiteral("font-weight:600;font-size:11pt;color:#111827;"));
    layout->addWidget(titleLabel);

    return card;
}

} // namespace

HelpDialog::HelpDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("快速上手 - AegisHMI"));
    setMinimumSize(720, 620);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(12);

    auto* title = new QLabel(QStringLiteral("3 分钟完成第一次测试"));
    title->setStyleSheet(QStringLiteral("font-size:16pt;font-weight:600;color:#263238;"));
    root->addWidget(title);
    root->addWidget(makeHint(QStringLiteral(
        "按下面的顺序操作即可。界面里每个控件都带有悬停提示（鼠标停留 1 秒可看到说明）。")));
    root->addWidget(makeHint(QStringLiteral(
        "顶部状态条常驻显示：链路在线数、未恢复报警（点击进报警页）、当前工单和登录用户。"
        "左侧导航栏切换页面，当前页有高亮与标题说明。")));

    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto* content = new QWidget();
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 4, 0);
    contentLayout->setSpacing(12);

    // ---- 第一步到第五步 ----
    QFrame* stepsCard = makeCard(QStringLiteral("操作流程"));
    auto* stepsLayout = qobject_cast<QVBoxLayout*>(stepsCard->layout());
    auto* stepsGrid = new QGridLayout();
    stepsGrid->setHorizontalSpacing(12);
    stepsGrid->setVerticalSpacing(12);
    stepsGrid->setColumnStretch(1, 1);
    addStep(stepsGrid, 0, QStringLiteral("1"), QStringLiteral("连接设备（监控页）"),
            QStringLiteral("在左侧“设备列表”选中一台设备 → 核对协议与地址 → 点击“连接”。"
                           "列表里 ● 表示在线、○ 表示离线、⚠ 表示有未恢复报警。"));
    addStep(stepsGrid, 1, QStringLiteral("2"), QStringLiteral("确认数据（监控页）"),
            QStringLiteral("右侧波形实时刷新；“链路健康”表显示每条链路的在线状态、"
                           "最近入站延迟与累计帧数。勾选“报文诊断（HEX）”可查看原始收发报文。"));
    addStep(stepsGrid, 2, QStringLiteral("3"), QStringLiteral("执行测试（自动测试页）"),
            QStringLiteral("点击“加载方案…”选择测试方案 → 填写/扫描 SN（回车即开始）→ "
                           "点击“开始测试”。“单步调试”可逐步排查配置问题。"));
    addStep(stepsGrid, 3, QStringLiteral("4"), QStringLiteral("查看结果（历史 / 趋势 / SPC）"),
            QStringLiteral("历史页按 SN/工单/方案检索并导出 PDF/CSV；趋势页看秒级包络；"
                           "SPC 页查看 Cp/Cpk 与控制图判异。"));
    addStep(stepsGrid, 4, QStringLiteral("5"), QStringLiteral("生产协同（生产 / 报警 / 班组）"),
            QStringLiteral("生产看板看工单、产量、良率与 MES 状态；报警页确认异常；"
                           "班组页完成开班与交接班。"));
    stepsLayout->addLayout(stepsGrid);
    contentLayout->addWidget(stepsCard);

    // ---- 页签地图 ----
    QFrame* mapCard = makeCard(QStringLiteral("功能地图"));
    auto* mapLayout = qobject_cast<QVBoxLayout*>(mapCard->layout());
    auto* mapGrid = new QGridLayout();
    mapGrid->setHorizontalSpacing(18);
    mapGrid->setVerticalSpacing(7);
    mapGrid->setColumnStretch(1, 1);
    int row = 0;
    addTabRow(mapGrid, row++, QStringLiteral("生产"), QStringLiteral("工单切换、产量/良率、MES 上传状态、工步耗时 Top"));
    addTabRow(mapGrid, row++, QStringLiteral("监控"), QStringLiteral("设备连接、实时波形、链路健康、捕获/点检/报文诊断、运行日志"));
    addTabRow(mapGrid, row++, QStringLiteral("自动测试"), QStringLiteral("加载/编辑方案、SN 与料号校验、执行与单步调试"));
    addTabRow(mapGrid, row++, QStringLiteral("历史"), QStringLiteral("按 SN/工单/方案/判定检索，导出 PDF/CSV，查看步骤明细"));
    addTabRow(mapGrid, row++, QStringLiteral("趋势"), QStringLiteral("秒级包络 + 多通道叠加对比与快捷时间范围"));
    addTabRow(mapGrid, row++, QStringLiteral("SPC"), QStringLiteral("Cp/Cpk、I-MR 控制图、判异与 Andon 联动"));
    addTabRow(mapGrid, row++, QStringLiteral("班组"), QStringLiteral("班次与交接班记录、操作员资质、当班汇总"));
    addTabRow(mapGrid, row++, QStringLiteral("插件"), QStringLiteral("外部插件：版本/能力、清单状态、自检与目录管理"));
    addTabRow(mapGrid, row++, QStringLiteral("报警"), QStringLiteral("阈值报警确认/搁置，页签标题显示未恢复数量"));
    addTabRow(mapGrid, row++, QStringLiteral("审计"), QStringLiteral("登录、连接、测试、复核等操作留痕（仅管理员可见）"));
    addTabRow(mapGrid, row++, QStringLiteral("用户"), QStringLiteral("账号、角色与口令管理（仅管理员可见）"));
    mapLayout->addLayout(mapGrid);
    contentLayout->addWidget(mapCard);

    // ---- 常见问题 ----
    QFrame* faqCard = makeCard(QStringLiteral("常见问题"));
    auto* faqLayout = qobject_cast<QVBoxLayout*>(faqCard->layout());
    faqLayout->addWidget(makeHint(QStringLiteral(
        "• 连接不上：确认模拟器/仪器已启动、地址端口一致；观察员角色不能连接设备。")));
    faqLayout->addWidget(makeHint(QStringLiteral(
        "• 设备列表为空：检查 configs/devices/fleet.json，或环境变量 AEGIS_FLEET_CONFIG。")));
    faqLayout->addWidget(makeHint(QStringLiteral(
        "• “开始测试”灰色：尚未加载方案、没有操作权限，或已有测试在运行。")));
    faqLayout->addWidget(makeHint(QStringLiteral(
        "• 参数不可修改：连接期间会锁定参数，断开后即可编辑。")));
    contentLayout->addWidget(faqCard);
    contentLayout->addStretch(1);

    scroll->setWidget(content);
    root->addWidget(scroll, 1);

    // ---- 底部：不再提示 + 关闭 ----
    auto* footer = new QHBoxLayout();
    skipBox_ = new QCheckBox(QStringLiteral("启动时不再自动显示"));
    skipBox_->setChecked(QSettings().value(kDismissedKey, false).toBool());
    footer->addWidget(skipBox_);
    footer->addStretch(1);

    auto* closeButton = makeButton(QStringLiteral("知道了"), QStringLiteral("primary"));
    closeButton->setDefault(true);
    footer->addWidget(closeButton);
    root->addLayout(footer);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
}

void HelpDialog::showIfNeeded(QWidget* parent)
{
    QSettings settings;
    if (settings.value(kDismissedKey, false).toBool())
        return;

    HelpDialog dialog(parent);
    dialog.exec();
}

void HelpDialog::done(int result)
{
    QSettings settings;
    settings.setValue(kDismissedKey, skipBox_ != nullptr && skipBox_->isChecked());
    QDialog::done(result);
}

} // namespace aegis::ui
