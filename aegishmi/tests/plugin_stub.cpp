// 测试替身：模拟用户插件的行为（独立进程 + JSON stdio）。
// 用法：plugin_stub.exe <pass|fail|timeout|crash|badjson>

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <cstdio>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("pass");

    // 读取一行输入（主程序写入的步骤上下文 JSON）。
    QByteArray line;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), stdin) != nullptr) {
        line.append(buffer);
        if (line.endsWith('\n'))
            break;
    }
    const QJsonObject input = QJsonDocument::fromJson(line.trimmed()).object();
    Q_UNUSED(input);

    if (mode == QLatin1String("crash"))
        return 3;
    if (mode == QLatin1String("timeout"))
        QThread::sleep(3);
    if (mode == QLatin1String("badjson")) {
        std::fputs("not a json\n", stdout);
        std::fflush(stdout);
        return 0;
    }

    QJsonObject output;
    if (mode == QLatin1String("fail")) {
        output.insert(QStringLiteral("pass"), false);
        output.insert(QStringLiteral("message"), QStringLiteral("电压超出范围"));
    } else {
        output.insert(QStringLiteral("pass"), true);
        output.insert(QStringLiteral("message"), QStringLiteral("ok"));
        QJsonObject measures;
        measures.insert(QStringLiteral("value"), 123.5);
        output.insert(QStringLiteral("measures"), measures);
    }

    std::fputs("stub log\n", stderr);
    const QByteArray data = QJsonDocument(output).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(data.constData(), 1, static_cast<std::size_t>(data.size()), stdout);
    std::fflush(stdout);
    return 0;
}
