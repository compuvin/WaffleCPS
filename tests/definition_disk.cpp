#include "../src/codeplugdefinitions.h"
#include "../src/codeplugdefinition.h"
#include "../src/ziparchive.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <stdexcept>

static void check(bool value, const QString &error) { if (!value) throw std::runtime_error(error.toStdString()); }
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QString error;
        if (argc == 2) {
            const QString mode = QString::fromLocal8Bit(argv[1]);
            if (mode == "catalog") {
                const auto filters = CodeplugDefinitions::importFileFilter(&error).split(";;");
                check(error.isEmpty(), error);
                const QString folder = qEnvironmentVariable("WAFFLE_DEFINITIONS_DIR");
                for (const auto &file : QDir(folder).entryList({"*.wcp"}, QDir::Files, QDir::Name)) {
                    QString choice;
                    for (const auto &filter : filters) if (filter.contains('[' + file + ']')) choice = filter;
                    check(!choice.isEmpty(), "Definition missing from import dialog: " + file);
                    check(CodeplugDefinitions::definitionForFilter(choice, &error) == file, "Wrong explicit definition selection");
                    CodeplugDefinition definition;
                    check(CodeplugDefinition::load(QDir(folder).filePath(file), &definition, &error), file + ": " + error);
                    QJsonObject binary;
                    check(definition.inspect(definition.defaultCodeplug(), &binary, &error), file + ": " + error);
                    QProcess inspector;
                    const QString python = QStringLiteral(R"PY(
import json, sys, zipfile
sys.path.insert(0, sys.argv[1])
import inspect_codeplug as inspector
definition = inspector.load_definition(sys.argv[2])
with zipfile.ZipFile(sys.argv[2]) as archive:
    template = archive.read(definition['creation']['template'])
representation = next(r for r in definition['representations'] if r['name'] == definition['creation']['representation'])
image, _ = inspector.decode_representation(template, representation)
reader = inspector.DefinitionReader(image, definition)
checks = inspector.check_recognition(reader, definition) + inspector.check_layout(reader, definition)
decoded = reader.decode_available_fields_and_tables()
checks += reader.reference_checks()
assert not reader.errors, reader.errors
assert all(c['status'] == 'match' for c in checks if c['required']), checks
for table in decoded['tables'].values():
    table.pop('complete', None)
    table.pop('selected_indices', None)
    table.pop('address', None)
    for row in table['records']:
        row.pop('complete', None)
print(json.dumps(decoded))
)PY");
                    inspector.start("python3", {"-B", "-c", python, QStringLiteral(WAFFLE_SOURCE_DIR) + "/tools", QDir(folder).filePath(file)});
                    check(inspector.waitForFinished(30000), "Python inspector comparison timed out: " + file);
                    const auto pythonError = inspector.readAllStandardError();
                    check(inspector.exitStatus() == QProcess::NormalExit && inspector.exitCode() == 0,
                          file + ": Python inspector failed: " + QString::fromUtf8(pythonError));
                    const auto pythonDecoded = QJsonDocument::fromJson(inspector.readAllStandardOutput()).object();
                    check(!pythonDecoded.isEmpty() && binary == pythonDecoded,
                          file + ": C++ binary fields/records differ from inspect_codeplug.py");
                    const auto decoded = binary["tables"].toObject();
                    check(!decoded.isEmpty(), "No binary tables decoded: " + file);
                    ImportedCodeplug plug; plug.definitionFile = "sentinel";
                    const bool imported = CodeplugDefinitions::read(definition.defaultCodeplug(), &plug, &error, file);
                    if (file == "cs7000-m17-plus.wcp" || file == "cs750.wcp"
                        || file == "cs800.wcp" || file == "opengd77-128k.wcp") {
                        check(imported && plug.definitionFile == file, "Import lost selected definition: " + error);
                        if (file == "cs800.wcp") {
                            QFile sample(QStringLiteral(WAFFLE_SOURCE_DIR) + "/Codeplugs/CS800-KB1JDX 2017-01-22.rdb");
                            check(sample.open(QIODevice::ReadOnly), "Cannot open CS800 sample");
                            const QByteArray original = sample.readAll();
                            ImportedCodeplug cs800;
                            check(CodeplugDefinitions::read(original, &cs800, &error, file), error);
                            QByteArray unchanged;
                            check(CodeplugDefinitions::write(original, cs800, cs800.radioName, cs800.radioId,
                                                             &unchanged, &error), error);
                            check(unchanged == original, "Unedited CS800 export changed bytes");
                            check(!cs800.channels.isEmpty(), "CS800 sample has no editable channels");
                            cs800.channels[0].name = "CS800 EXPORT";
                            QByteArray edited;
                            check(CodeplugDefinitions::write(original, cs800, cs800.radioName, cs800.radioId,
                                                             &edited, &error), error);
                            check(edited != original, "Edited CS800 export did not change bytes");
                            ImportedCodeplug reimported;
                            check(CodeplugDefinitions::read(edited, &reimported, &error, file), error);
                            check(reimported.channels[0].name == "CS800 EXPORT",
                                  "Edited CS800 channel did not survive reimport");
                        }
                        if (file == "opengd77-128k.wcp") {
                            QFile sample(QStringLiteral(WAFFLE_SOURCE_DIR) + "/Codeplugs/DM1701_Berlin.g77");
                            check(sample.open(QIODevice::ReadOnly), "Cannot open Berlin OpenGD77 sample");
                            const QByteArray original = sample.readAll();
                            ImportedCodeplug berlin;
                            check(CodeplugDefinitions::read(original, &berlin, &error, file), error);
                            check(berlin.channels.size() == 119 && berlin.contacts.size() == 28
                                  && berlin.zones.size() == 5, "Berlin import counts changed");
                            QByteArray unchanged;
                            check(CodeplugDefinitions::write(original, berlin, berlin.radioName, berlin.radioId,
                                                             &unchanged, &error), error);
                            check(unchanged == original, "Unedited Berlin export changed bytes");
                            berlin.channels[0].name = "GD EXPORT TEST";
                            berlin.channels[0].setPowerValue("level 5");
                            QByteArray edited;
                            check(CodeplugDefinitions::write(original, berlin, berlin.radioName, berlin.radioId,
                                                             &edited, &error), error);
                            check(edited != original && edited.size() == original.size(),
                                  "Edited Berlin export did not produce a same-size codeplug");
                            ImportedCodeplug reimported;
                            check(CodeplugDefinitions::read(edited, &reimported, &error, file), error);
                            check(reimported.channels[0].name == "GD EXPORT TEST"
                                  && reimported.channels[0].powerValue() == "level 5",
                                  "Edited Berlin channel did not survive reimport");
                        }
                    } else {
                        check(!imported && plug.definitionFile == "sentinel" && error.contains(file)
                              && error.contains("creation.construction"), "Reset-only definition was guessed or hidden: " + error);
                        plug.definitionFile = file;
                        QByteArray output("sentinel");
                        check(!CodeplugDefinitions::write(definition.defaultCodeplug(), plug, "TEST", 3109101, &output, &error)
                              && output == "sentinel" && error.contains(file) && error.contains("construction"),
                              "Undefined export rules were guessed: " + error);
                    }
                    qInfo().noquote() << file << ": C++ binary results equal inspect_codeplug.py; explicit import" << (imported ? "succeeded" : "rejected with definition requirements");
                }
                check(filters.join(";;").contains("cs800.wcp") && filters.join(";;").contains("*.rdb"),
                      "CS800 definition or its declared extension was hidden");
                return 0;
            }
            if (mode == "ambiguous") {
                CodeplugDefinition definition;
                check(CodeplugDefinition::load(qEnvironmentVariable("WAFFLE_DEFINITIONS_DIR") + "/cs7000-m17-plus.wcp",
                                              &definition, &error), error);
                ImportedCodeplug plug;
                check(!CodeplugDefinitions::read(definition.defaultCodeplug(), &plug, &error)
                      && error.contains("More than one"), "Ambiguous definitions were silently selected");
                check(CodeplugDefinitions::read(definition.defaultCodeplug(), &plug, &error, "alias.wcp")
                      && plug.definitionFile == "alias.wcp", "Explicit definition selection was ignored: " + error);
                QByteArray output;
                check(CodeplugDefinitions::write(definition.defaultCodeplug(), plug, "SELECTED", 3109101, &output, &error),
                      "Export did not retain explicitly selected definition: " + error);
                QString extension;
                check(!CodeplugDefinitions::exportFileFilter(output, &extension, &error, plug.definitionFile).isEmpty(),
                      "Export dialog lost explicitly selected definition: " + error);
                return 0;
            }
            const auto base = CodeplugDefinitions::defaultCodeplug(&error);
            if (mode == "missing") { check(base.isEmpty() && !error.isEmpty(), "Missing external definition was bypassed"); return 0; }
            check(!base.isEmpty(), error);
            ImportedCodeplug plug;
            bool ok = CodeplugDefinitions::read(base, &plug, &error); check(ok, error);
            QString extension = "sentinel";
            const auto exportFilter = CodeplugDefinitions::exportFileFilter(base, &extension, &error);
            if (mode == "noextension") {
                check(exportFilter.isEmpty() && extension == "sentinel"
                      && error.contains("no filename extension"), "Missing extension was guessed: " + error);
                return 0;
            }
            const QString expectedExtension = mode == "denied" ? ".disk" : ".rdbx";
            check(extension == expectedExtension && exportFilter.contains('*' + expectedExtension),
                  "Export dialog ignored the external definition: " + error);
            QString warnings;
            const auto filter = CodeplugDefinitions::importFileFilter(&error, &warnings);
            check(filter.contains(mode == "denied" ? "*.disk" : "*.rdbx"), "External extension change was ignored");
            if (mode == "rejected") {
                check(warnings.contains("broken.wcp"), "Rejected definition was hidden");
                ImportedCodeplug untouched;
                check(!CodeplugDefinitions::read("unrecognized", &untouched, &error)
                      && error.contains("broken.wcp"), "Import failure hid rejected definitions");
            }
            const auto report = CodeplugDefinitions::diskLoadReport();
            check(report.contains("cs7000-m17-plus.wcp") && report.contains("definition.json SHA-256:")
                  && report.contains("Restart WaffleCPS"), "Disk provenance or cache explanation missing");
            QByteArray output("sentinel");
            ok = CodeplugDefinitions::write(base, plug, "DISK TEST", 3109101, &output, &error);
            if (mode == "denied") check(!ok && output == "sentinel" && error.contains("Radio ID"), "External writer limit was ignored: " + error);
            else {
                check(ok, error);
                ok = CodeplugDefinitions::read(output, &plug, &error);
                check(ok && plug.radioName == "DISK TEST" && plug.radioId == 3109101, "Disk-defined export failed reimport: " + error);
            }
            return 0;
        }
        QTemporaryDir directory; check(directory.isValid(), "No temporary directory");
        {
            QProcess child;
            auto catalogEnvironment = QProcessEnvironment::systemEnvironment();
            catalogEnvironment.insert("WAFFLE_DEFINITIONS_DIR", QStringLiteral(WAFFLE_SOURCE_DIR) + "/definitions");
            child.setProcessEnvironment(catalogEnvironment);
            child.start(QCoreApplication::applicationFilePath(), {"catalog"});
            check(child.waitForFinished(60000), "Multi-definition catalog test timed out");
            check(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                  QString::fromUtf8(child.readAllStandardOutput()) + QString::fromUtf8(child.readAllStandardError()));
            qInfo() << "All repository definitions are selectable and parsed through the same binary reader";
        }
        const QString folder = directory.filePath("definitions");
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("WAFFLE_DEFINITIONS_DIR", folder);
        const auto run = [&](const QString &mode) {
            QProcess child; child.setProcessEnvironment(environment);
            child.start(QCoreApplication::applicationFilePath(), {mode});
            check(child.waitForFinished(30000), "Disk-definition child timed out");
            check(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0, QString::fromUtf8(child.readAllStandardError()));
            if (mode != "missing") {
                QMap<QString, QByteArray> diskEntries;
                const QString diskPath = folder + "/cs7000-m17-plus.wcp";
                check(ZipArchive::read(diskPath, &diskEntries, &error), error);
                const auto hash = QCryptographicHash::hash(diskEntries["definition.json"], QCryptographicHash::Sha256).toHex();
                const auto output = child.readAllStandardOutput();
                check(output.contains("Reading definition FILE: " + diskPath.toUtf8())
                      && output.contains("definition.json SHA-256: " + hash)
                      && output.contains("Import uses cached definition loaded from FILE:"),
                      "stdout did not identify the actual disk file and JSON hash");
                qInfo().noquote() << mode << "verified disk definition.json SHA-256:" << hash;
            }
        };
        run("missing");
        check(QDir().mkpath(folder), "Cannot create test folder");
        QMap<QString, QByteArray> entries;
        bool ok = ZipArchive::read(QStringLiteral(WAFFLE_SOURCE_DIR) + "/definitions/cs7000-m17-plus.wcp", &entries, &error); check(ok, error);
        const QString path = folder + "/cs7000-m17-plus.wcp";
        ok = ZipArchive::write(path, entries, &error); check(ok, error);
        run("normal");
        ok = ZipArchive::write(folder + "/alias.wcp", entries, &error); check(ok, error);
        run("ambiguous");
        check(QFile::remove(folder + "/alias.wcp"), "Cannot remove alias definition fixture");
        {
            QFile broken(folder + "/broken.wcp");
            check(broken.open(QIODevice::WriteOnly) && broken.write("invalid archive") == 15,
                  "Cannot create rejected definition fixture");
        }
        run("rejected");
        check(QFile::remove(folder + "/broken.wcp"), "Cannot remove rejected definition fixture");
        auto changed = entries;
        auto json = QJsonDocument::fromJson(changed["definition.json"]).object();
        auto creation = json["creation"].toObject(), plan = creation["construction"].toObject();
        auto limits = plan["validate_target"].toObject(), id = limits["radio_id"].toObject();
        id["maximum"] = 3109100; limits["radio_id"] = id; plan["validate_target"] = limits;
        creation["construction"] = plan; json["creation"] = creation;
        auto representations = json["representations"].toArray();
        auto representation = representations[0].toObject(); representation["extension"] = ".disk";
        representations[0] = representation; json["representations"] = representations;
        changed["definition.json"] = QJsonDocument(json).toJson();
        ok = ZipArchive::write(path, changed, &error); check(ok, error);
        run("denied");
        representation.remove("extension");
        representations[0] = representation; json["representations"] = representations;
        changed["definition.json"] = QJsonDocument(json).toJson();
        ok = ZipArchive::write(path, changed, &error); check(ok, error);
        run("noextension");
        ok = ZipArchive::write(path, entries, &error); check(ok, error);
        run("normal");
        check(QFile::remove(path), "Cannot remove temporary definition");
        run("missing");
        qInfo() << "Runtime disk loading proved: missing file fails, external filter/limit changes take effect, restored file succeeds without rebuilding";
        return 0;
    } catch (const std::exception &e) { qCritical().noquote() << e.what(); return 1; }
}
