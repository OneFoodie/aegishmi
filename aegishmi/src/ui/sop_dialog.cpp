#include "ui/sop_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace aegis::ui {

SopDialog::SopDialog(const core::SopDocument& document, bool requireRead,
                     QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("作业指导书（SOP）"));
    resize(720, 640);

    auto* root = new QVBoxLayout(this);

    auto* title = new QLabel(document.title.isEmpty()
                                 ? QStringLiteral("作业指导书")
                                 : document.title);
    QFont titleFont = title->font();
    titleFont.setPointSize(titleFont.pointSize() + 3);
    titleFont.setBold(true);
    title->setFont(titleFont);
    root->addWidget(title);

    if (!document.version.isEmpty()) {
        auto* version = new QLabel(QStringLiteral("版本：%1　产品：%2")
                                       .arg(document.version,
                                            document.product.isEmpty()
                                                ? QStringLiteral("通用")
                                                : document.product));
        version->setStyleSheet(QStringLiteral("color:#7f8c8d"));
        root->addWidget(version);
    }

    // 步骤用有序列表渲染，避免手工拼序号。
    QString html = QStringLiteral("<ol style=\"margin-left:-16px;\">");
    for (const QString& step : document.steps) {
        QString escaped = step.toHtmlEscaped();
        html += QStringLiteral("<li style=\"margin-bottom:8px;\">%1</li>").arg(escaped);
    }
    html += QStringLiteral("</ol>");

    auto* browser = new QTextBrowser();
    browser->setHtml(html);
    root->addWidget(browser, 1);

    // 可选配图：按宽度缩放，避免超宽对话框。
    if (!document.imagePath.isEmpty() && QFileInfo::exists(document.imagePath)) {
        QImage image(document.imagePath);
        if (!image.isNull()) {
            auto* imageLabel = new QLabel();
            imageLabel->setPixmap(QPixmap::fromImage(
                image.scaledToWidth(640, Qt::SmoothTransformation)));
            imageLabel->setAlignment(Qt::AlignCenter);
            auto* scroll = new QScrollArea();
            scroll->setWidget(imageLabel);
            scroll->setWidgetResizable(true);
            scroll->setMaximumHeight(240);
            root->addWidget(scroll);
        }
    }

    readBox_ = new QCheckBox(QStringLiteral("我已阅读并确认按上述步骤操作"));
    root->addWidget(readBox_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("开始测试"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    root->addWidget(buttons);

    if (requireRead) {
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        connect(readBox_, &QCheckBox::toggled, this, [buttons](bool checked) {
            buttons->button(QDialogButtonBox::Ok)->setEnabled(checked);
        });
    } else {
        readBox_->setChecked(true);
    }

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

} // namespace aegis::ui
