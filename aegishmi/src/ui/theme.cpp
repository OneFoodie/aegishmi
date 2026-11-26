#include "ui/theme.h"

#include <QApplication>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QStyle>
#include <QVariant>
#include <QPolygonF>

namespace aegis::ui {

namespace {

// 设计基调：深青作为主操作色（工业控制常用、与报警红/警告橙区分明显），
// 浅灰画布 + 白卡片，保证长时间盯屏不刺眼。
const char* const kStyleSheet = R"QSS(
QWidget {
    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
    font-size: 10pt;
    color: #1f2937;
}
QMainWindow, QDialog { background: #eceff1; }
QTabWidget::pane {
    border: 1px solid #e3e7ec;
    border-radius: 8px;
    background: #f9fafb;
    top: -1px;
}
QTabBar::tab {
    background: transparent;
    padding: 7px 16px;
    margin-right: 2px;
    border: none;
    border-bottom: 2px solid transparent;
    color: #4b5563;
}
QTabBar::tab:hover { color: #37474f; }
QTabBar::tab:selected {
    color: #37474f;
    font-weight: 600;
    border-bottom: 2px solid #37474f;
}
QFrame[role="card"] {
    background: #ffffff;
    border: 1px solid #e3e7ec;
    border-radius: 8px;
}
QLabel[role="section"] {
    font-weight: 600;
    font-size: 10.5pt;
    color: #111827;
    padding-top: 2px;
}
QLabel[role="hint"] { color: #6b7280; font-size: 9pt; }
QLabel[role="status"] {
    border-radius: 10px;
    padding: 3px 10px;
    background: #e5e7eb;
    color: #374151;
    font-weight: 600;
}
QLabel[role="status"][tone="ok"] { background: #e3ece7; color: #2f5d4a; }
QLabel[role="status"][tone="warn"] { background: #fdf1dc; color: #8a5300; }
QLabel[role="status"][tone="error"] { background: #fbe7e5; color: #a3271f; }
QPushButton {
    background: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 6px;
    padding: 5px 12px;
    color: #1f2937;
}
QPushButton:hover { background: #f3f4f6; border-color: #9ca3af; }
QPushButton:pressed { background: #e5e7eb; }
QPushButton:disabled { color: #9ca3af; background: #f9fafb; border-color: #e5e7eb; }
QPushButton[role="primary"] {
    background: #37474f;
    border-color: #37474f;
    color: #ffffff;
    font-weight: 600;
}
QPushButton[role="primary"]:hover { background: #263238; border-color: #263238; }
QPushButton[role="primary"]:pressed { background: #1c262b; }
QPushButton[role="primary"]:disabled {
    background: #cbd5e1; border-color: #cbd5e1; color: #f8fafc;
}
QPushButton[role="danger"] {
    background: #ffffff;
    border-color: #fca5a5;
    color: #b3261e;
    font-weight: 600;
}
QPushButton[role="danger"]:hover { background: #fef2f2; border-color: #ef4444; }
QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
    background: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 6px;
    padding: 4px 8px;
    min-height: 20px;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus {
    border: 1px solid #37474f;
}
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QDoubleSpinBox:disabled {
    background: #f3f4f6;
    color: #6b7280;
}
QListWidget {
    background: #ffffff;
    border: 1px solid #e3e7ec;
    border-radius: 6px;
    padding: 3px;
}
QListWidget::item { padding: 5px 6px; border-radius: 4px; }
QListWidget::item:selected { background: #eceff1; color: #263238; }
QTableWidget {
    background: #ffffff;
    border: 1px solid #e3e7ec;
    border-radius: 6px;
    gridline-color: #eef1f4;
    selection-background-color: #eceff1;
    selection-color: #263238;
}
QHeaderView::section {
    background: #f8fafc;
    color: #4b5563;
    border: none;
    border-bottom: 1px solid #e3e7ec;
    padding: 5px 6px;
    font-weight: 600;
}
QPlainTextEdit, QTextEdit {
    background: #ffffff;
    border: 1px solid #e3e7ec;
    border-radius: 6px;
}
QPlainTextEdit[role="log"] {
    font-family: Consolas, "Courier New", monospace;
    font-size: 9pt;
    background: #fbfcfd;
}
QProgressBar {
    border: 1px solid #e3e7ec;
    border-radius: 6px;
    background: #f3f4f6;
    text-align: center;
    min-height: 16px;
}
QProgressBar::chunk { background: #37474f; border-radius: 5px; }
QCheckBox { spacing: 6px; }
QScrollArea { background: transparent; border: none; }
QScrollBar:vertical { width: 10px; background: transparent; margin: 2px; }
QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 5px; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: #94a3b8; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal { height: 10px; background: transparent; margin: 2px; }
QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 5px; min-width: 30px; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QFrame[role="topbar"] {
    background: #ffffff;
    border: none;
    border-bottom: 1px solid #e3e7ec;
}
QLabel[role="appTitle"] {
    font-size: 11pt;
    font-weight: 700;
    color: #263238;
}
QLabel[role="chip"] {
    background: #f1f5f9;
    border-radius: 9px;
    padding: 3px 9px;
    color: #334155;
    font-size: 9pt;
}
QLabel[role="chip"][tone="ok"] { background: #e3ece7; color: #2f5d4a; }
QLabel[role="chip"][tone="warn"] { background: #fdf1dc; color: #8a5300; }
QLabel[role="chip"][tone="error"] { background: #fbe7e5; color: #a3271f; }
QPushButton[role="chip"] {
    background: #f1f5f9;
    border: none;
    border-radius: 9px;
    padding: 3px 9px;
    color: #334155;
    font-size: 9pt;
    font-weight: 400;
}
QPushButton[role="chip"]:hover { background: #e2e8f0; }
QPushButton[role="chip"][tone="error"] { background: #fbe7e5; color: #a3271f; font-weight: 600; }
QPushButton[role="alarmBanner"] {
    background: #fdf1dc;
    border: 1px solid #fde68a;
    border-radius: 0;
    padding: 7px 14px;
    text-align: left;
    color: #8a5300;
    font-weight: 600;
}
QPushButton[role="alarmBanner"]:hover { background: #fde68a; }
QFrame[role="metric"] {
    background: #ffffff;
    border: 1px solid #e3e7ec;
    border-radius: 8px;
}
QLabel[role="metricTitle"] { color: #6b7280; font-size: 9pt; }
QLabel[role="metricValue"] { color: #111827; font-size: 18pt; font-weight: 600; }
QLabel[role="metricValue"][tone="ok"] { color: #2e7d5b; }
QLabel[role="metricValue"][tone="warn"] { color: #a35f00; }
QLabel[role="metricValue"][tone="error"] { color: #b3261e; }
QLabel[role="faceplateName"] { font-size: 12pt; font-weight: 700; color: #111827; }
/* ---- 工业风框架：深色导航栏 + 控制台顶栏 + 页面标题 ---- */
QFrame[role="rail"] {
    background: qlineargradient(x1:0, y1:0, x2:0.6, y2:1,
                                stop:0 #0a121a, stop:1 #132230);
    border-right: 1px solid #1e2c38;
}
QLabel[role="brand"] {
    color: #e8f4fa;
    font-size: 15pt;
    font-weight: 700;
    letter-spacing: 3px;
}
QLabel[role="brandSub"] { color: #7f97a8; font-size: 8.5pt; letter-spacing: 1px; }
QPushButton[role="nav"] {
    background: transparent;
    border: none;
    border-left: 3px solid transparent;
    border-radius: 0;
    color: #a9bccb;
    text-align: left;
    padding: 9px 12px;
    font-size: 10pt;
}
QPushButton[role="nav"]:hover { background: rgba(255, 255, 255, 0.06); color: #e8f4fa; }
QPushButton[role="nav"]:checked {
    background: rgba(34, 211, 238, 0.12);
    border-left: 3px solid #22d3ee;
    color: #eafcff;
    font-weight: 600;
}
QPushButton[role="nav"]:pressed { background: rgba(34, 211, 238, 0.18); }
QFrame[role="topbar"] {
    background: #0b131b;
    border: none;
    border-bottom: 1px solid #1e2c38;
}
QFrame[role="topbar"] QLabel[role="chip"] {
    background: rgba(255, 255, 255, 0.08);
    color: #c7d6e1;
}
QFrame[role="topbar"] QLabel[role="chip"][tone="ok"] { background: #12352a; color: #86e0b3; }
QFrame[role="topbar"] QLabel[role="chip"][tone="warn"] { background: #3a2a12; color: #ffcb7d; }
QFrame[role="topbar"] QLabel[role="chip"][tone="error"] { background: #3a1618; color: #ff9d9d; }
QFrame[role="topbar"] QPushButton {
    background: transparent;
    border: 1px solid #2a3a48;
    color: #c7d6e1;
    border-radius: 6px;
}
QFrame[role="topbar"] QPushButton:hover { background: rgba(255, 255, 255, 0.07); border-color: #3d5262; }
QFrame[role="topbar"] QPushButton[role="chip"] {
    border: none;
    background: rgba(255, 255, 255, 0.08);
}
QFrame[role="topbar"] QPushButton[role="chip"][tone="error"] {
    background: #3a1618;
    color: #ff9d9d;
}
QLabel[role="pageTitle"] { font-size: 17pt; font-weight: 700; color: #0f1b26; }
QLabel[role="pageSubtitle"] { color: #5b6b7a; font-size: 9.5pt; }
QWidget[role="contentArea"] { background: #eef1f4; }
QTabWidget[role="content"]::pane { border: none; background: transparent; }
QToolTip {
    background: #111827;
    color: #f9fafb;
    border: 1px solid #374151;
    border-radius: 4px;
    padding: 4px 7px;
    font-size: 9pt;
}
)QSS";

} // namespace

void applyTheme(QApplication& app)
{
    app.setStyleSheet(QString::fromUtf8(kStyleSheet));
}

QLabel* makeHint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("hint"));
    label->setWordWrap(true);
    return label;
}

QLabel* makeSectionTitle(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setProperty("role", QStringLiteral("section"));
    return label;
}

QPushButton* makeButton(const QString& text, const QString& role, QWidget* parent)
{
    auto* button = new QPushButton(text, parent);
    if (!role.isEmpty())
        button->setProperty("role", role);
    return button;
}

QString toneText(StatusTone tone)
{
    switch (tone) {
    case StatusTone::Ok:
        return QStringLiteral("ok");
    case StatusTone::Warn:
        return QStringLiteral("warn");
    case StatusTone::Error:
        return QStringLiteral("error");
    case StatusTone::Neutral:
        break;
    }
    return QStringLiteral("neutral");
}

void setTone(QWidget* widget, StatusTone tone)
{
    if (widget == nullptr)
        return;
    widget->setProperty("tone", toneText(tone));
    // 属性变化后需要重新计算样式，否则颜色不会立即切换。
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setStatusTone(QLabel* label, StatusTone tone)
{
    setTone(label, tone);
}

void setButtonRole(QPushButton* button, const QString& role)
{
    if (button == nullptr)
        return;
    button->setProperty("role", role);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
}

// -----------------------------------------------------------------------------
// 工业风线性图标：用 QPainter 现画，避免引入图片资源与额外依赖。
// 统一 40×40 画布（约 20px 视觉尺寸），线宽 3，圆头圆角。
// -----------------------------------------------------------------------------
QIcon navIcon(const QString& kind, const QColor& color)
{
    QPixmap pixmap(40, 40);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(color, 3.0);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    if (kind == QLatin1String("production")) {
        painter.drawLine(QPointF(11, 30), QPointF(11, 20));
        painter.drawLine(QPointF(20, 30), QPointF(20, 14));
        painter.drawLine(QPointF(29, 30), QPointF(29, 23));
        painter.drawLine(QPointF(9, 32), QPointF(31, 32));
    } else if (kind == QLatin1String("monitor")) {
        QPolygonF wave;
        wave << QPointF(9, 22) << QPointF(14, 22) << QPointF(17, 15)
             << QPointF(20, 28) << QPointF(23, 19) << QPointF(26, 22)
             << QPointF(31, 22);
        painter.drawPolyline(wave);
    } else if (kind == QLatin1String("test")) {
        painter.drawEllipse(QRectF(9, 9, 22, 22));
        QPolygonF play;
        play << QPointF(17, 15) << QPointF(17, 25) << QPointF(26, 20);
        painter.setBrush(color);
        painter.drawPolygon(play);
        painter.setBrush(Qt::NoBrush);
    } else if (kind == QLatin1String("history")) {
        painter.drawEllipse(QRectF(9, 9, 22, 22));
        painter.drawLine(QPointF(20, 14), QPointF(20, 21));
        painter.drawLine(QPointF(20, 21), QPointF(25, 24));
    } else if (kind == QLatin1String("trend")) {
        QPolygonF curve;
        curve << QPointF(9, 30) << QPointF(15, 24) << QPointF(20, 26)
              << QPointF(26, 18) << QPointF(31, 14);
        painter.drawPolyline(curve);
        painter.drawLine(QPointF(31, 14), QPointF(25, 14));
        painter.drawLine(QPointF(31, 14), QPointF(31, 20));
    } else if (kind == QLatin1String("spc")) {
        painter.drawLine(QPointF(9, 31), QPointF(32, 31));
        painter.drawLine(QPointF(9, 31), QPointF(9, 9));
        QPolygonF curve;
        curve << QPointF(10, 27) << QPointF(15, 23) << QPointF(19, 16)
              << QPointF(24, 16) << QPointF(28, 23) << QPointF(31, 27);
        painter.drawPolyline(curve);
        QPen dashed = pen;
        dashed.setStyle(Qt::DashLine);
        dashed.setWidth(2);
        painter.setPen(dashed);
        painter.drawLine(QPointF(9, 13), QPointF(32, 13));
        painter.drawLine(QPointF(9, 28), QPointF(32, 28));
    } else if (kind == QLatin1String("shift")) {
        painter.drawEllipse(QRectF(10, 10, 10, 10));
        painter.drawArc(QRectF(6, 21, 18, 14), 0, 180 * 16);
        painter.drawEllipse(QRectF(23, 13, 8, 8));
        painter.drawArc(QRectF(20, 23, 15, 12), 0, 180 * 16);
    } else if (kind == QLatin1String("alarm")) {
        QPainterPath bell;
        bell.moveTo(12, 27);
        bell.lineTo(12, 20);
        bell.cubicTo(12, 13, 28, 13, 28, 20);
        bell.lineTo(28, 27);
        bell.closeSubpath();
        painter.drawPath(bell);
        painter.drawLine(QPointF(9, 27), QPointF(31, 27));
        painter.drawLine(QPointF(20, 9), QPointF(20, 12));
        painter.drawEllipse(QRectF(18, 28, 4, 4));
    } else if (kind == QLatin1String("plugin")) {
        // 插头：两脚 + 本体 + 线缆
        painter.drawLine(QPointF(14, 9), QPointF(14, 15));
        painter.drawLine(QPointF(26, 9), QPointF(26, 15));
        painter.drawRoundedRect(QRectF(11, 15, 18, 12), 3, 3);
        painter.drawLine(QPointF(20, 27), QPointF(20, 33));
    } else if (kind == QLatin1String("audit")) {
        QPainterPath shield;
        shield.moveTo(20, 9);
        shield.lineTo(30, 13);
        shield.lineTo(28, 24);
        shield.lineTo(20, 31);
        shield.lineTo(12, 24);
        shield.lineTo(10, 13);
        shield.closeSubpath();
        painter.drawPath(shield);
        painter.drawLine(QPointF(15, 20), QPointF(19, 24));
        painter.drawLine(QPointF(19, 24), QPointF(26, 16));
    } else { // user
        painter.drawEllipse(QRectF(15, 9, 10, 10));
        painter.drawArc(QRectF(10, 21, 20, 14), 0, 180 * 16);
    }

    painter.end();
    return QIcon(pixmap);
}

} // namespace aegis::ui
