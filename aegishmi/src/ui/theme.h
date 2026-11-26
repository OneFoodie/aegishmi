#pragma once // 头文件保护

#include <QColor>
#include <QIcon>
#include <QString>

class QApplication;
class QLabel;
class QPushButton;
class QWidget;

namespace aegis::ui {

// 状态色语义：中性 / 正常 / 警告 / 故障。用于状态胶囊、提示标签。
enum class StatusTone {
    Neutral,
    Ok,
    Warn,
    Error,
};

// 把枚举转成样式表属性值（neutral / ok / warn / error）。
QString toneText(StatusTone tone);

// 全局主题：字体、配色、控件样式。必须在创建窗口前调用一次。
void applyTheme(QApplication& app);

// 小型说明文字（灰色、自动换行），放在控件下方解释"这是什么/怎么用"。
QLabel* makeHint(const QString& text, QWidget* parent = nullptr);

// 分区标题（加粗），用于把左侧面板分成"连接 / 查看 / 工具"几段。
QLabel* makeSectionTitle(const QString& text, QWidget* parent = nullptr);

// 按钮样式：role = ""（普通）/ "primary"（主操作）/ "danger"（断开、停止）。
QPushButton* makeButton(const QString& text, const QString& role = QString(),
                        QWidget* parent = nullptr);

// 状态胶囊：设置文本并切换颜色语义（中性灰 / 在线绿 / 警告橙 / 故障红）。
void setStatusTone(QLabel* label, StatusTone tone);

// 通用色调属性：给任意控件设置 tone，样式表按 [tone="ok"] 等选择器生效。
void setTone(QWidget* widget, StatusTone tone);

// 运行期切换按钮的视觉角色（例如"连接"→"断开"时从主操作变成危险操作）。
void setButtonRole(QPushButton* button, const QString& role);

// 工业风线性图标：kind = production/monitor/test/history/trend/spc/shift/alarm/audit/user。
QIcon navIcon(const QString& kind, const QColor& color = QColor(0xA9, 0xBC, 0xCB));

} // namespace aegis::ui
