#pragma once // 头文件保护

#include <QDialog>

class QCheckBox;

namespace aegis::ui {

// =============================================================================
// HelpDialog —— 快速上手指南
//
// 用途：解决"界面功能不明确、没有提示"的问题。首次启动自动弹出，
//       之后可从监控页的"使用说明"按钮再次打开。
// 内容：5 步操作流程 + 页签功能地图 + 常见问题。
// =============================================================================
class HelpDialog : public QDialog {
    Q_OBJECT

public:
    explicit HelpDialog(QWidget* parent = nullptr);

    // 首次启动（未选择"不再提示"）时自动显示。
    static void showIfNeeded(QWidget* parent);

protected:
    void done(int result) override;

private:
    QCheckBox* skipBox_ = nullptr;
};

} // namespace aegis::ui
