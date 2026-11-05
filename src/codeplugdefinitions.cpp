#include "codeplugdefinitions.h"
#include "codeplugdefinition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cstdio>

#include "ziparchive.h"

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

struct Version {
    QString id;
    QJsonObject match;
    CodeplugDefinition definition;
};

struct Entry {
    QString fileName;
    QString model;
    QVector<Version> versions;
    int defaultVersion = 0;
    QString loadError;
    QString editorError;
    bool loaded = false;
    bool companionWrd = false;

    const CodeplugDefinition &definition() const { return versions[defaultVersion].definition; }

    QStringList extensions() const
    {
        QStringList result;
        for (const auto &version : versions)
            for (const auto &extension : version.definition.extensions())
                if (!result.contains(extension)) result.append(extension);
        return result;
    }

    QString filter() const
    {
        QStringList patterns;
        if (loaded)
            for (const auto &extension : extensions()) patterns.append('*' + extension);
        const QString label = loaded ? model : QStringLiteral("Failed definition");
        // '*' displays files for definitions without a declared extension. It
        // does not assign an extension or waive any parser requirement.
        return QStringLiteral("%1 [%2]%3 (%4)")
            .arg(label, fileName,
                 loaded && patterns.isEmpty() ? QStringLiteral(" - no extension declared") : QString(),
                 patterns.isEmpty() ? QStringLiteral("*") : patterns.join(' '));
    }
};

bool loadEntry(const QString &path, Entry *entry, QString *error)
{
    QMap<QString, QByteArray> members;
    if (!ZipArchive::read(path, &members, error)) return false;
    if (!members.contains(QStringLiteral("manifest.json"))) {
        Version version;
        if (!CodeplugDefinition::load(path, &version.definition, error)) return false;
        entry->model = version.definition.modelName();
        entry->versions.append(std::move(version));
        return true;
    }

    const QByteArray manifestBytes = members.value(QStringLiteral("manifest.json"));
    if (manifestBytes.size() > 4 * 1024 * 1024) { *error = QStringLiteral("Manifest is oversized."); return false; }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral("Invalid manifest JSON: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject manifest = document.object();
    const QJsonArray versions = manifest.value(QStringLiteral("versions")).toArray();
    const QString defaultId = manifest.value(QStringLiteral("default_version")).toString();
    entry->model = manifest.value(QStringLiteral("radio")).toString();
    if (manifest.value(QStringLiteral("package_version")) != QJsonValue(1)
        || entry->model.isEmpty() || defaultId.isEmpty() || versions.isEmpty() || versions.size() > 32) {
        *error = QStringLiteral("Manifest lacks a supported version, radio, default version, or versions.");
        return false;
    }
    QSet<QString> ids;
    for (const QJsonValue &item : versions) {
        if (!item.isObject()) { *error = QStringLiteral("Invalid manifest version."); return false; }
        const QJsonObject itemObject = item.toObject();
        Version version;
        version.id = itemObject.value(QStringLiteral("id")).toString();
        version.match = itemObject.value(QStringLiteral("match")).toObject();
        const QString member = itemObject.value(QStringLiteral("definition")).toString();
        if (version.id.isEmpty() || ids.contains(version.id) || member.isEmpty()
            || !itemObject.value(QStringLiteral("match")).isObject() || version.match.isEmpty()) {
            *error = QStringLiteral("Invalid or duplicate manifest version ID, definition, or match.");
            return false;
        }
        for (auto it = version.match.begin(); it != version.match.end(); ++it)
            if (it.key().isEmpty() || it.value().isNull() || it.value().isArray() || it.value().isObject()) {
                *error = QStringLiteral("Manifest match values must be named scalar values.");
                return false;
            }
        if (!CodeplugDefinition::loadMember(path, member, &version.definition, error)) return false;
        if (version.definition.document().value(QStringLiteral("recognition")).toObject()
                .value(QStringLiteral("model_value")).toString() != entry->model) {
            *error = QStringLiteral("Manifest radio differs from version model: %1").arg(version.id);
            return false;
        }
        QJsonObject decoded;
        QString detail;
        if (!version.definition.inspect(version.definition.defaultCodeplug(), &decoded, &detail)) {
            *error = QStringLiteral("Manifest version %1 starter is invalid: %2").arg(version.id, detail);
            return false;
        }
        const QJsonObject raw = decoded.value(QStringLiteral("raw_fields")).toObject();
        for (auto it = version.match.begin(); it != version.match.end(); ++it) {
            if (raw.value(it.key()) != it.value()) {
                *error = QStringLiteral("Manifest version %1 starter does not match %2.").arg(version.id, it.key());
                return false;
            }
        }
        ids.insert(version.id);
        const bool isDefault = version.id == defaultId;
        entry->versions.append(std::move(version));
        if (isDefault) entry->defaultVersion = entry->versions.size() - 1;
    }
    if (!ids.contains(defaultId)) { *error = QStringLiteral("Manifest default version is undefined."); return false; }
    return true;
}

struct Catalog {
    QVector<Entry> entries;
    QVector<int> defaultDefinitions;
    QStringList rejected;
    QString error;
    QString directoryPath;

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
        directoryPath = directory.absolutePath();
        logDefinition(QStringLiteral("Loading .wcp files from disk directory: %1").arg(directory.absolutePath()));
        const auto files = directory.entryList({QStringLiteral("*.wcp")}, QDir::Files, QDir::Name);
        for (const auto &name : files) {
            Entry entry;
            entry.fileName = name;
            entry.companionWrd = QFileInfo::exists(directory.filePath(QFileInfo(name).completeBaseName() + ".wrd"));
            entry.loaded = loadEntry(directory.filePath(name), &entry, &entry.loadError);
            if (!entry.loaded) {
                rejected.append(name + ": " + entry.loadError);
                logDefinition(QStringLiteral("REJECTED disk definition %1: %2")
                              .arg(directory.absoluteFilePath(name), entry.loadError));
            } else {
                for (const auto &version : entry.versions) {
                    logDefinition(QStringLiteral("LOADED binary definition successfully\n%1")
                                  .arg(version.definition.diskProvenance()));
                    ImportedCodeplug unused;
                    QString detail;
                    if (!version.definition.read(version.definition.defaultCodeplug(), &unused, &detail)) {
                        entry.editorError = QStringLiteral("%1: %2").arg(version.id, detail);
                        logDefinition(QStringLiteral("%1: editor import unavailable: %2").arg(name, entry.editorError));
                        break;
                    }
                }
                if (entry.editorError.isEmpty()) defaultDefinitions.append(entries.size());
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

struct Selection {
    const Entry *entry = nullptr;
    const Version *version = nullptr;
};

QString selectionName(const Selection &selection)
{
    return selection.entry->fileName + (selection.version->id.isEmpty()
        ? QString() : QStringLiteral(" [%1]").arg(selection.version->id));
}

Selection select(const QByteArray &bytes, const QString &fileName, QString *error)
{
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) { fail(error, loaded.error); return {}; }
    QVector<Selection> exact;
    QVector<Selection> fallback;
    QStringList reasons;
    for (const auto &entry : loaded.entries) {
        if (!fileName.isEmpty() && entry.fileName != fileName) continue;
        if (!entry.loaded) {
            const QString reason = entry.fileName + ": " + entry.loadError;
            if (!fileName.isEmpty()) { fail(error, reason); return {}; }
            reasons.append(reason);
            continue;
        }
        if (entry.versions.size() == 1 && entry.versions.first().id.isEmpty()) {
            QString detail;
            if (entry.definition().matchesModel(bytes, &detail))
                exact.append({&entry, &entry.versions.first()});
            else reasons.append(entry.fileName + ": " + detail);
            continue;
        }
        bool mappedVersion = false;
        for (const auto &version : entry.versions) {
            if (!version.definition.matchesRawFields(bytes, version.match)) continue;
            mappedVersion = true;
            QJsonObject decoded;
            QString detail;
            if (!version.definition.inspect(bytes, &decoded, &detail)) {
                reasons.append(QStringLiteral("%1 [%2]: %3").arg(entry.fileName, version.id, detail));
                continue;
            }
            exact.append({&entry, &version});
        }
        if (!mappedVersion) {
            const Version &version = entry.versions[entry.defaultVersion];
            ImportedCodeplug unused;
            QString detail;
            if (version.definition.read(bytes, &unused, &detail))
                fallback.append({&entry, &version});
            else reasons.append(QStringLiteral("%1 default [%2]: %3").arg(entry.fileName, version.id, detail));
        }
    }
    const auto &matches = exact.isEmpty() ? fallback : exact;
    if (matches.size() > 1) {
        QStringList names;
        for (const auto &match : matches) names.append(selectionName(match));
        fail(error, QStringLiteral("More than one definition recognizes this file: %1. Select a definition explicitly in the import dialog.")
                    .arg(names.join(", ")));
        return {};
    }
    if (matches.isEmpty()) {
        fail(error, fileName.isEmpty()
             ? QStringLiteral("No loaded definition matches this file.\n%1").arg(reasons.join('\n'))
             : QStringLiteral("Selected definition could not read this file: %1\n%2").arg(fileName, reasons.join('\n')));
        return {};
    }
    return matches.first();
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
            for (const auto &extension : entry.extensions()) all.append('*' + extension);
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
        QStringList provenances;
        for (const auto &version : entry.versions) provenances.append(version.definition.diskProvenance());
        QString status = provenances.join(QStringLiteral("\n"));
        if (!entry.editorError.isEmpty()) status += "\nEditor import unavailable: " + entry.editorError;
        if (entry.extensions().isEmpty()) status += "\nNo filename extension declared.";
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
            {QStringLiteral("model"), entry.loaded ? entry.model : entry.fileName},
            {QStringLiteral("fileName"), entry.fileName},
            {QStringLiteral("hasIcon"), entry.loaded && !entry.definition().iconPng().isEmpty()},
            {QStringLiteral("available"), available},
            {QStringLiteral("reason"), entry.loaded ? entry.editorError : entry.loadError}
        });
    }
    return options;
}

QVariantList CodeplugDefinitions::radioReadOptions()
{
    QVariantList options;
    for (const auto &entry : catalog().entries) {
        if (!entry.loaded || !entry.companionWrd) continue;
        options.append(QVariantMap{
            {QStringLiteral("model"), entry.model},
            {QStringLiteral("fileName"), entry.fileName},
            {QStringLiteral("hasIcon"), !entry.definition().iconPng().isEmpty()}
        });
    }
    return options;
}

QByteArray CodeplugDefinitions::iconPng(const QString &definitionFile)
{
    for (const auto &entry : catalog().entries)
        if (entry.loaded && entry.fileName == definitionFile)
            return entry.definition().iconPng();
    return {};
}

QString CodeplugDefinitions::modelName(const QString &definitionFile)
{
    for (const auto &entry : catalog().entries)
        if (entry.loaded && entry.fileName == definitionFile)
            return entry.model;
    return {};
}

bool CodeplugDefinitions::hasCompanionWrd(const QString &definitionFile)
{
    for (const auto &entry : catalog().entries)
        if (entry.loaded && entry.fileName == definitionFile)
            return entry.companionWrd;
    return false;
}

QString CodeplugDefinitions::companionWrdPath(const QString &definitionFile)
{
    if (!hasCompanionWrd(definitionFile)) return {};
    return QDir(catalog().directoryPath).absoluteFilePath(QFileInfo(definitionFile).completeBaseName() + ".wrd");
}

QString CodeplugDefinitions::defaultDefinitionFile()
{
    const auto &loaded = catalog();
    QVector<int> defaults;
    for (int index : loaded.defaultDefinitions)
        if (loaded.entries[index].definition().defaultForNew()) defaults.append(index);
    if (defaults.isEmpty()) defaults = loaded.defaultDefinitions;
    return defaults.size() == 1 ? loaded.entries[defaults.first()].fileName : QString();
}

QString CodeplugDefinitions::exportFileFilter(const QByteArray &source,
                                            QString *extension, QString *error,
                                            const QString &definitionFile)
{
    if (!extension) { fail(error, "Missing export extension output."); return {}; }
    const auto selected = select(source, definitionFile, error);
    if (!selected.entry) return {};
    QString selectedExtension, detail;
    if (!selected.version->definition.exportExtension(source, &selectedExtension, &detail)) {
        fail(error, QStringLiteral("%1: export failed: %2").arg(selectionName(selected), detail));
        return {};
    }
    logDefinition(QStringLiteral("Export dialog uses cached definition loaded from FILE:\n%1")
                  .arg(selected.version->definition.diskProvenance()));
    *extension = selectedExtension;
    if (error) error->clear();
    return QStringLiteral("%1 (*%2)").arg(selected.entry->model, selectedExtension);
}

bool CodeplugDefinitions::read(const QByteArray &bytes, ImportedCodeplug *result, QString *error,
                              const QString &definitionFile)
{
    if (!result) return fail(error, "Missing codeplug output.");
    const auto selected = select(bytes, definitionFile, error);
    if (!selected.entry) return false;
    logDefinition(QStringLiteral("Import uses cached definition loaded from FILE:\n%1")
                  .arg(selected.version->definition.diskProvenance()));
    QString detail;
    ImportedCodeplug imported;
    if (!selected.version->definition.read(bytes, &imported, &detail))
        return fail(error, QStringLiteral("%1: import failed: %2").arg(selectionName(selected), detail));
    imported.definitionFile = selected.entry->fileName;
    imported.definitionVersion = selected.version->id;
    *result = std::move(imported);
    if (error) error->clear();
    return true;
}

bool CodeplugDefinitions::encodeRadioImage(const QByteArray &decoded, const QString &definitionFile,
                                           QByteArray *encoded, QString *extension, QString *error)
{
    const auto selected = select(decoded, definitionFile, error);
    if (!selected.entry) return false;
    QByteArray image;
    if (!selected.version->definition.decodedImage(decoded, &image, error)) return false;
    return selected.version->definition.encodeRadioImage(image, encoded, extension, error);
}

QByteArray CodeplugDefinitions::defaultCodeplug(QString *error)
{
    const auto &loaded = catalog();
    if (!loaded.error.isEmpty()) { fail(error, loaded.error); return {}; }
    QVector<int> defaults;
    for (int index : loaded.defaultDefinitions)
        if (loaded.entries[index].definition().defaultForNew()) defaults.append(index);
    if (defaults.isEmpty()) defaults = loaded.defaultDefinitions;
    if (defaults.size() != 1) {
        fail(error, QStringLiteral("Select a target radio before creating a codeplug."));
        return {};
    }
    if (error) error->clear();
    return loaded.entries[defaults.first()].definition().defaultCodeplug();
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
        return entry.definition().defaultCodeplug();
    }
    fail(error, QStringLiteral("Selected definition file is not available: %1").arg(definitionFile));
    return {};
}

bool CodeplugDefinitions::write(const QByteArray &source, const ImportedCodeplug &target,
                               const QString &radioName, int radioId, QByteArray *result, QString *error)
{
    if (!result) return fail(error, "Missing codeplug output.");
    const auto selected = select(source, target.definitionFile, error);
    if (!selected.entry) return false;
    if (!target.definitionVersion.isEmpty() && target.definitionVersion != selected.version->id)
        return fail(error, QStringLiteral("Project definition version does not match source codeplug: %1")
                    .arg(target.definitionVersion));
    logDefinition(QStringLiteral("Export writer uses cached definition loaded from FILE:\n%1")
                  .arg(selected.version->definition.diskProvenance()));
    QString detail;
    if (!selected.version->definition.write(source, target, radioName, radioId, result, &detail))
        return fail(error, QStringLiteral("%1: export failed: %2").arg(selectionName(selected), detail));
    if (error) error->clear();
    return true;
}
