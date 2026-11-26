#include "test/flow_catalog.h"

#include "test/flow_loader.h"
#include "test/flow_validator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace aegis::test {

QString FlowCatalog::defaultDirectory()
{
    const QString fromEnv = qEnvironmentVariable("AEGIS_FLOW_DIR");
    if (!fromEnv.isEmpty() && QFileInfo::exists(fromEnv))
        return fromEnv;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/configs/flows"),
        appDir + QStringLiteral("/../../configs/flows"),
    };
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path))
            return QDir(path).absolutePath();
    }
    // 都没有时返回第一候选，界面会显示"目录不存在"。
    return candidates.first();
}

FlowEntry FlowCatalog::describe(const QString& path)
{
    FlowEntry entry;
    entry.path = QFileInfo(path).absoluteFilePath();

    const FlowLoader::LoadResult loaded = FlowLoader::loadFromFile(path);
    if (!loaded.ok()) {
        entry.error = loaded.errors.isEmpty()
                          ? QStringLiteral("加载失败")
                          : QStringLiteral("%1: %2")
                                .arg(loaded.errors.first().fieldPath,
                                     loaded.errors.first().message);
        return entry;
    }

    const QVector<FlowIssue> issues = FlowValidator::validate(loaded.flow);
    if (!issues.isEmpty()) {
        entry.error = QStringLiteral("%1: %2")
                          .arg(issues.first().fieldPath, issues.first().message);
        return entry;
    }

    entry.id = loaded.flow.id;
    entry.name = loaded.flow.name;
    entry.product = loaded.flow.product;
    entry.description = loaded.flow.description;
    entry.version = loaded.flow.version;
    entry.checksum = loaded.flow.checksum;
    entry.steps = loaded.flow.steps.size();
    entry.valid = true;
    return entry;
}

QVector<FlowEntry> FlowCatalog::scan(const QString& directory)
{
    QVector<FlowEntry> entries;
    QDir dir(directory);
    if (!dir.exists())
        return entries;

    const QStringList files =
        dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& file : files)
        entries.append(describe(dir.absoluteFilePath(file)));
    return entries;
}

} // namespace aegis::test
