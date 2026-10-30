#include "codeplugdefinitions.h"
#include "codeplugdefinition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <cstdio>

namespace {
void logDefinition(const QString &message)
{
    std::fprintf(stdout, "[definitions] %s\n", qUtf8Printable(message));
    std::fflush(stdout);
}

bool fail(QString *error, const QString &message)
{
    logDefinition(QStringLiteral("ERROR: %1").arg(message));
    if (error) *error = message;
    return false;
}

struct Entry {
    QString fileName;
    CodeplugDefinition definition;
    QString loadError;
    QString editorError;
    bool loaded = false;
    bool companionWrd = false;

    QString filter() const
    {
        QStringList patterns;
        if (loaded)
            for (const auto &extension : definition.extensions()) patterns.append('*' + extension);
        const QString label = loaded ? definition.modelName() : QStringLiteral("Failed definition");
        // '*' displays files for definitions without a declared extension. It
        // does not assign an extension or waive any parser requirement.
        return QStringLiteral("%1 [%2]%3 (%4)")
            .arg(label, fileName,
                 loaded && patterns.isEmpty() ? QStringLiteral(" - no extension declared") : QString(),
                 patterns.isEmpty() ? QStringLiteral("*") : patterns.join(' '));
    }
};

struct Catalog {
    QVector<Entry> entries;
    QVector<int> defaultDefinitions;
    QStringList rejected;
    QString error;

    Catalog()
    {
        QString folder = qEnvironmentVariable("WAFFLE_DEFINITIONS_DIR");
        if (folder.isEmpty()) {
            const QString executable = QCoreApplication::applicationDirPath();
            const QStringList candidates{executable + "/definitions", executable + "/../definitions",
                                         QDir::currentPath() + "/definitions"};
            for (const auto &candidate : candidates)
                if (QDir(candidate).exists()) { folder = candidate; break; }
        }
        const QDir directory(folder);
        if (folder.isEmpty() || !directory.exists()) {
            error = QStringLiteral("The definitions folder was not found. Place it beside Waffle or set WAFFLE_DEFINITIONS_DIR.");
            return;
        }
        logDefinition(QStringLiteral("Loading .wcp files from disk directory: %1").arg(directory.absolutePath()));
        const auto files = directory.entryList({QStringLiteral("*.wcp")}, QDir::Files, QDir::Name);
        for (const auto &name : files) {
            Entry entry;
            entry.fileName = name;
            entry.companionWrd = QFileInfo::exists(directory.filePath(QFileInfo(name).completeBaseName() + ".wrd"));
            entry.loaded = CodeplugDefinition::load(directory.filePath(name), &entry.definition, &entry.loadError);
            if (!entry.loaded) {
                rejected.append(name + ": " + entry.loadError);
                logDefinition(QStringLiteral("REJECTED disk definition %1: %2")
                              .arg(directory.absoluteFilePath(name), entry.loadError));
            } else {
                logDefinition(QStringLiteral("LOADED binary definition successfully\n%1").arg(entry.definition.diskProvenance()));
                ImportedCodeplug unused;
                if (entry.definition.read(entry.definition.defaultCodeplug(), &unused, &entry.editorError))
                    defaultDefinitions.append(entries.size());
                else logDefinition(QStringLiteral("%1: editor import unavailable: %2").arg(name, entry.editorError));
            }
            // Retain every file, including rejected definitions, for explicit selection.
            entries.append(std::move(entry));
        }
        if (entries.isEmpty()) error = QStringLiteral("No .wcp definition files were found in %1.").arg(directory.absolutePath());
    }
};

const Catalog &catalog()
{
    static const Catalog value;
    return value;
}

const Entry *select(const QByteArray &bytes, const QString &fileName, QString *error)
{
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) { fail(error, loaded.error); return nullptr; }
    const Entry *matched = nullptr;
    QStringList reasons;
    for (const auto &entry : loaded.entries) {
        if (!fileName.isEmpty() && entry.fileName != fileName) continue;
        if (!entry.loaded) {
            const QString reason = entry.fileName + ": " + entry.loadError;
            if (!fileName.isEmpty()) { fail(error, reason); return nullptr; }
            reasons.append(reason);
            continue;
        }
        QString detail;
        if (!entry.definition.matchesModel(bytes, &detail)) {
            const QString reason = entry.fileName + ": " + detail;
            if (!fileName.isEmpty()) { fail(error, reason); return nullptr; }
            reasons.append(reason);
            continue;
        }
        if (matched) {
            fail(error, QStringLiteral("More than one definition recognizes this file: %1, %2. Select a definition explicitly in the import dialog.")
                        .arg(matched->fileName, entry.fileName));
            return nullptr;
        }
        matched = &entry;
    }
    if (!matched) {
        fail(error, fileName.isEmpty()
             ? QStringLiteral("No loaded definition matches this file.\n%1").arg(reasons.join('\n'))
             : QStringLiteral("Selected definition file is not available: %1").arg(fileName));
        return nullptr;
    }
    return matched;
}
} // namespace

QString CodeplugDefinitions::importFileFilter(QString *error, QString *warnings)
{
    const auto &loaded = catalog();
    if (warnings) *warnings = loaded.rejected.join('\n');
    if (!loaded.error.isEmpty()) { fail(error, loaded.error); return {}; }
    QStringList all, filters;
    for (const auto &entry : loaded.entries) {
        if (entry.loaded)
            for (const auto &extension : entry.definition.extensions()) all.append('*' + extension);
        filters.append(entry.filter());
    }
    all.removeDuplicates();
    all.sort();
    // The aggregate choice detects a definition from file contents. Each named
    // choice below forces that exact definition, including one that failed loading.
    filters.prepend(all.isEmpty() ? QStringLiteral("Detect definition from file (*)")
        : QStringLiteral("Detect definition from file (%1)").arg(all.join(' ')));
    filters.append(QStringLiteral("All files (*)"));
    if (error) error->clear();
    return filters.join(QStringLiteral(";;"));
}

QString CodeplugDefinitions::definitionForFilter(const QString &filter, QString *error)
{
    const auto &loaded = catalog();
    for (const auto &entry : loaded.entries) {
        if (entry.filter() != filter) continue;
        if (error) error->clear();
        return entry.fileName;
    }
    QString filterError;
    const auto filters = importFileFilter(&filterError).split(QStringLiteral(";;"));
    if (!filterError.isEmpty()) { fail(error, filterError); return {}; }
    if (filter == filters.first() || filter == QStringLiteral("All files (*)")) {
        if (error) error->clear();
        return {};
    }
    fail(error, "Unknown definition filter: " + filter);
    return {};
}

QString CodeplugDefinitions::diskLoadReport()
{
    const auto &loaded = catalog();
    QStringList report{QStringLiteral("Definitions are read from .wcp FILES on disk at first use.\n"
                                     "This process then uses the loaded definitions in memory.\n"
                                     "Restart WaffleCPS after editing a definition file.")};
    for (const auto &entry : loaded.entries) {
        if (!entry.loaded) {
            report.append(entry.fileName + ": binary definition failed to load: " + entry.loadError);
            continue;
        }
        QString status = entry.definition.diskProvenance();
        if (!entry.editorError.isEmpty()) status += "\nEditor import unavailable: " + entry.editorError;
        if (entry.definition.extensions().isEmpty()) status += "\nNo filename extension declared.";
        report.append(status);
    }
    if (!loaded.error.isEmpty()) report.append(loaded.error);
    return report.join(QStringLiteral("\n\n"));
}

QVariantList CodeplugDefinitions::newProjectOptions()
{
    QVariantList options;
    for (const auto &entry : catalog().entries) {
        const bool available = entry.loaded && entry.editorError.isEmpty();
        options.append(QVariantMap{
            {QStringLiteral("model"), entry.loaded ? entry.definition.modelName() : entry.fileName},
            {QStringLiteral("fileName"), entry.fileName},
            {QStringLiteral("hasIcon"), entry.loaded && !entry.definition.iconPng().isEmpty()},
            {QStringLiteral("available"), available},
            {QStringLiteral("reason"), entry.loaded ? entry.editorError : entry.loadError}
        });
    }
    return options;
}

QByteArray CodeplugDefinitions::iconPng(const QString &definitionFile)
{
    for (const auto &entry : catalog().entries)
        if (entry.loaded && entry.fileName == definitionFile)
            return entry.definition.iconPng();
    return {};
}

bool CodeplugDefinitions::hasCompanionWrd(const QString &definitionFile)
{
    for (const auto &entry : catalog().entries)
        if (entry.loaded && entry.fileName == definitionFile)
            return entry.companionWrd;
    return false;
}

QString CodeplugDefinitions::exportFileFilter(const QByteArray &source,
                                            QString *extension, QString *error,
                                            const QString &definitionFile)
{
    if (!extension) { fail(error, "Missing export extension output."); return {}; }
    const auto *entry = select(source, definitionFile, error);
    if (!entry) return {};
    QString selectedExtension, detail;
    if (!entry->definition.exportExtension(source, &selectedExtension, &detail)) {
        fail(error, QStringLiteral("%1: export failed: %2").arg(entry->fileName, detail));
        return {};
    }
    logDefinition(QStringLiteral("Export dialog uses cached definition loaded from FILE:\n%1")
                  .arg(entry->definition.diskProvenance()));
    *extension = selectedExtension;
    if (error) error->clear();
    return QStringLiteral("%1 (*%2)").arg(entry->definition.modelName(), selectedExtension);
}

bool CodeplugDefinitions::read(const QByteArray &bytes, ImportedCodeplug *result, QString *error,
                              const QString &definitionFile)
{
    if (!result) return fail(error, "Missing codeplug output.");
    const auto *entry = select(bytes, definitionFile, error);
    if (!entry) return false;
    logDefinition(QStringLiteral("Import uses cached definition loaded from FILE:\n%1")
                  .arg(entry->definition.diskProvenance()));
    QString detail;
    ImportedCodeplug imported;
    if (!entry->definition.read(bytes, &imported, &detail))
        return fail(error, QStringLiteral("%1: import failed: %2").arg(entry->fileName, detail));
    imported.definitionFile = entry->fileName;
    *result = std::move(imported);
    if (error) error->clear();
    return true;
}

QByteArray CodeplugDefinitions::defaultCodeplug(QString *error)
{
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) { fail(error, loaded.error); return {}; }
    QVector<int> defaults;
    for (int index : loaded.defaultDefinitions)
        if (loaded.entries[index].definition.defaultForNew()) defaults.append(index);
    if (defaults.isEmpty()) defaults = loaded.defaultDefinitions;
    if (defaults.size() != 1) {
        fail(error, QStringLiteral("Select a target radio before creating a codeplug."));
        return {};
    }
    if (error) error->clear();
    return loaded.entries[defaults.first()].definition.defaultCodeplug();
}

QByteArray CodeplugDefinitions::defaultCodeplug(const QString &definitionFile, QString *error)
{
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) { fail(error, loaded.error); return {}; }
    for (const auto &entry : loaded.entries) {
        if (entry.fileName != definitionFile) continue;
        if (!entry.loaded) { fail(error, entry.loadError); return {}; }
        if (!entry.editorError.isEmpty()) { fail(error, entry.editorError); return {}; }
        if (error) error->clear();
        return entry.definition.defaultCodeplug();
    }
    fail(error, QStringLiteral("Selected definition file is not available: %1").arg(definitionFile));
    return {};
}

bool CodeplugDefinitions::write(const QByteArray &source, const ImportedCodeplug &target,
                               const QString &radioName, int radioId, QByteArray *result, QString *error)
{
    if (!result) return fail(error, "Missing codeplug output.");
    const auto *entry = select(source, target.definitionFile, error);
    if (!entry) return false;
    logDefinition(QStringLiteral("Export writer uses cached definition loaded from FILE:\n%1")
                  .arg(entry->definition.diskProvenance()));
    QString detail;
    if (!entry->definition.write(source, target, radioName, radioId, result, &detail))
        return fail(error, QStringLiteral("%1: export failed: %2").arg(entry->fileName, detail));
    if (error) error->clear();
    return true;
}
