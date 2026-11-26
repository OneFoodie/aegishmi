#pragma once // 头文件保护

#include "core/station_config.h"

#include <QDialog>

class QCheckBox;

namespace aegis::ui {

// =============================================================================
// SopDialog —— 电子作业指导书（SOP）阅读确认
//
// 测试开始前按产品展示图文步骤；requireRead 为真时必须勾选"已阅读"才能继续。
// 关闭（取消）时测试不开始。
// =============================================================================
class SopDialog : public QDialog {
    Q_OBJECT

public:
    SopDialog(const core::SopDocument& document, bool requireRead,
              QWidget* parent = nullptr);

private:
    QCheckBox* readBox_ = nullptr;
};

} // namespace aegis::ui
