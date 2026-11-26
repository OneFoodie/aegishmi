#include "ui/work_order_dialog.h"

#include "test/flow_catalog.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace aegis::ui {

WorkOrderDialog::WorkOrderDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新建生产工单"));
    setModal(true);
    setMinimumWidth(460);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    orderEdit_ = new QLineEdit();
    orderEdit_->setPlaceholderText(QStringLiteral("例如 WO-20261125-001"));
    productEdit_ = new QLineEdit();
    productEdit_->setPlaceholderText(QStringLiteral("例如 PM-5V-10A"));
    planSpin_ = new QSpinBox();
    planSpin_->setRange(0, 1000000);
    planSpin_->setValue(100);
    planSpin_->setSpecialValueText(QStringLiteral("不限"));

    // 方案库：扫描 configs/flows，绑定的方案会在切换工单时自动加载。
    flowCombo_ = new QComboBox();
    flowCombo_->addItem(QStringLiteral("（不绑定，手工加载）"), QString());
    for (const test::FlowEntry& entry : test::FlowCatalog::scan(test::FlowCatalog::defaultDirectory())) {
        if (!entry.valid)
            continue;
        const QString text = QStringLiteral("%1　·　%2（%3 步）")
                                 .arg(entry.name, entry.product)
                                 .arg(entry.steps);
        flowCombo_->addItem(text, entry.path);
        const int index = flowCombo_->count() - 1;
        flowCombo_->setItemData(index, entry.description, Qt::ToolTipRole);
        flowCombo_->setItemData(index, entry.id, Qt::UserRole + 1);
    }

    form->addRow(QStringLiteral("工单号"), orderEdit_);
    form->addRow(QStringLiteral("产品"), productEdit_);
    form->addRow(QStringLiteral("计划数量"), planSpin_);
    form->addRow(QStringLiteral("测试方案"), flowCombo_);
    root->addLayout(form);

    errorLabel_ = new QLabel();
    errorLabel_->setStyleSheet(QStringLiteral("color:#c0392b"));
    errorLabel_->setWordWrap(true);
    root->addWidget(errorLabel_);

    auto* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    auto* cancelButton = new QPushButton(QStringLiteral("取消"));
    auto* okButton = new QPushButton(QStringLiteral("创建"));
    okButton->setDefault(true);
    buttons->addWidget(cancelButton);
    buttons->addWidget(okButton);
    root->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(okButton, &QPushButton::clicked, this, &WorkOrderDialog::accept);

    // 选方案时顺手填产品（仅当用户还没填产品）。
    connect(flowCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || !productEdit_->text().trimmed().isEmpty())
            return;
        const QString path = flowCombo_->itemData(index).toString();
        if (path.isEmpty())
            return;
        const test::FlowEntry entry = test::FlowCatalog::describe(path);
        if (!entry.product.isEmpty())
            productEdit_->setText(entry.product);
    });

    orderEdit_->setFocus();
}

QString WorkOrderDialog::orderNo() const
{
    return orderEdit_->text().trimmed();
}

QString WorkOrderDialog::product() const
{
    return productEdit_->text().trimmed();
}

int WorkOrderDialog::planQty() const
{
    return planSpin_->value();
}

QString WorkOrderDialog::flowPath() const
{
    return flowCombo_->currentData().toString();
}

void WorkOrderDialog::accept()
{
    if (orderNo().isEmpty()) {
        errorLabel_->setText(QStringLiteral("工单号不能为空"));
        orderEdit_->setFocus();
        return;
    }
    if (product().isEmpty()) {
        errorLabel_->setText(QStringLiteral("产品不能为空"));
        productEdit_->setFocus();
        return;
    }
    QDialog::accept();
}

} // namespace aegis::ui
