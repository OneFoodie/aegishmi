#pragma once // 头文件保护

#include <QDialog>

class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace aegis::ui {

// 新建生产工单：工单号 + 产品 + 计划数量 + 绑定测试方案（可选）。
class WorkOrderDialog : public QDialog {
    Q_OBJECT

public:
    explicit WorkOrderDialog(QWidget* parent = nullptr);

    QString orderNo() const;
    QString product() const;
    int planQty() const;
    QString flowPath() const; // 空表示不绑定

public slots:
    void accept() override; // 校验通过才关闭

private:
    QLineEdit* orderEdit_ = nullptr;
    QLineEdit* productEdit_ = nullptr;
    QSpinBox* planSpin_ = nullptr;
    QComboBox* flowCombo_ = nullptr;
    QLabel* errorLabel_ = nullptr;
};

} // namespace aegis::ui
