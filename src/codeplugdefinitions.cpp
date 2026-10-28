#include "codeplugdefinitions.h"
#include "codeplugdefinition.h"

#include <QDir>
#include <QResource>

// Keep resource initialization outside a namespace: Qt generates a global symbol.
static void initializeDefinitionResources()
{
    Q_INIT_RESOURCE(codeplug_definitions);
}

namespace {
struct Catalog {
    QVector<CodeplugDefinition> definitions;
    QString error;

    Catalog()
    {
        initializeDefinitionResources();
        const QDir directory(QStringLiteral(":/definitions"));
        const auto files = directory.entryList({QStringLiteral("*.wcp")}, QDir::Files, QDir::Name);
        for (const auto &name : files) {
            CodeplugDefinition definition;
            if (!CodeplugDefinition::load(directory.filePath(name), &definition, &error)) {
                error = QStringLiteral("Could not load bundled definition %1: %2").arg(name, error);
                definitions.clear();
                return;
            }
            definitions.append(std::move(definition));
        }
        if (definitions.isEmpty()) error = QStringLiteral("No supported codeplug definitions are available.");
    }
};

const Catalog &catalog()
{
    static const Catalog value;
    return value;
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}
} // namespace

QString CodeplugDefinitions::importFileFilter(QString *error)
{
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) {
        fail(error, loaded.error);
        return {};
    }
    QStringList all, filters;
    for (const auto &definition : loaded.definitions) {
        QStringList patterns;
        for (const auto &extension : definition.extensions()) patterns.append('*' + extension);
        if (patterns.isEmpty()) continue;
        all.append(patterns);
        filters.append(QStringLiteral("%1 (%2)").arg(definition.modelName(), patterns.join(' ')));
    }
    all.removeDuplicates();
    all.sort();
    if (all.isEmpty()) {
        fail(error, QStringLiteral("The bundled definitions declare no codeplug file extensions."));
        return {};
    }
    filters.prepend(QStringLiteral("All supported codeplugs (%1)").arg(all.join(' ')));
    filters.append(QStringLiteral("All files (*)"));
    if (error) error->clear();
    return filters.join(QStringLiteral(";;"));
}

bool CodeplugDefinitions::read(const QByteArray &bytes, ImportedCodeplug *result, QString *error)
{
    if (!result) return fail(error, QStringLiteral("Missing codeplug output."));
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) return fail(error, loaded.error);
    const CodeplugDefinition *matched = nullptr;
    for (const auto &definition : loaded.definitions) {
        if (!definition.matchesModel(bytes, nullptr)) continue;
        if (matched) return fail(error, QStringLiteral("More than one bundled definition matches this radio model."));
        matched = &definition;
    }
    if (!matched)
        return fail(error, QStringLiteral("No supported radio model was found in this file. The codeplug may be unsupported or damaged."));
    return matched->read(bytes, result, error);
}
