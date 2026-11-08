#include "../src/codeplugdefinitions.h"
#include "../src/radiodefinition.h"
#include "../src/radioexecutor.h"
#include "../src/ziparchive.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QUrl>
#include <stdexcept>

namespace {
void check(bool condition, const QString &message)
{
    if (!condition) throw std::runtime_error(message.toStdString());
}
QByteArray profile(QJsonObject json, const QString &id, int delay)
{
    json["id"] = id;
    auto methods = json["methods"].toObject();
    auto write = methods["write_codeplug"].toObject();
    write["codeplug_definition_id"] = id;
    methods["write_codeplug"] = write;
    json["methods"] = methods;
    auto polling = json["polling"].toObject();
    polling["delay_before_each_fetch_ms"] = delay;
    json["polling"] = polling;
    return QJsonDocument(json).toJson();
}
QString reference(const QString &path, const QString &version)
{
    QUrl url = QUrl::fromLocalFile(path);
    url.setFragment(version);
    return url.toString(QUrl::FullyEncoded);
}
int delay(const RadioDefinition &radio)
{
    return radio.document()["polling"].toObject()["delay_before_each_fetch_ms"].toInt();
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        QTemporaryDir directory(QDir::tempPath() + "/waffle profiles-XXXXXX");
        check(directory.isValid(), "Cannot create temporary profile directory");
        qputenv("WAFFLE_DEFINITIONS_DIR", directory.path().toUtf8());
        QString error;
        QMap<QString, QByteArray> versions, single;
        const QString source = QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/");
        check(ZipArchive::read(source + "cs750.wcp", &versions, &error), error);
        versions.remove("usb.json");
        check(ZipArchive::read(source + "cs800.wcp", &single, &error), error);
        const auto base = QJsonDocument::fromJson(single.value("usb.json")).object();
        check(!base.isEmpty(), "CS800 package has no USB profile");
        single.remove("usb.json");
        const auto shared = profile(base, "cs750", 20);
        const auto override = profile(base, "cs750", 21);
        const QString oldMember = "versions/d4-0-03/usb.json";
        const QString newMember = "versions/d4-3-01/usb.json";
        const auto package = [&](const QString &name, QMap<QString, QByteArray> members) {
            const QString path = directory.filePath(name);
            check(ZipArchive::write(path, members, &error), error);
            return path;
        };
        const auto external = [&](const QString &name, const QByteArray &json) {
            QFile file(directory.filePath(name));
            check(file.open(QIODevice::WriteOnly) && file.write(json) == json.size(), "Cannot write standalone fixture");
        };
        auto both = versions;
        both["usb.json"] = shared;
        both[oldMember] = override;
        both[newMember] = profile(base, "cs750-d4-3-01", 22);
        const QString sharedPath = package("shared #1.wcp", both);
        auto versionOnly = versions;
        versionOnly[oldMember] = override;
        package("version-only.wcp", versionOnly);
        auto broken = both;
        broken[oldMember] = "{";
        const QString brokenPath = package("broken.wcp", broken);
        external("broken.wrd", shared);
        auto brokenRoot = versions;
        brokenRoot["usb.json"] = "{";
        const QString brokenRootPath = package("broken-root.wcp", brokenRoot);
        external("broken-root.wrd", shared);
        auto singleRoot = single;
        singleRoot["usb.json"] = QJsonDocument(base).toJson();
        const QString singlePath = package("single.wcp", singleRoot);
        package("legacy.wcp", single);
        external("legacy.wrd", QJsonDocument(base).toJson());
        package("none.wcp", single);

        RadioDefinition radio;
        CodeplugDefinition codeplug;
        QString wcpPath;
        check(RadioDefinition::loadProfile(sharedPath, &radio, &codeplug, &wcpPath, &error), error);
        check(delay(radio) == 20 && codeplug.id() == "cs750-d4-3-01" && wcpPath == sharedPath,
              "Initial read used a version override or wrong starter");
        RadioDefinition reloaded;
        CodeplugDefinition reloadedCodeplug;
        check(RadioDefinition::loadProfile(radio.sourcePath(), &reloaded, &reloadedCodeplug, nullptr, &error), error);
        check(delay(reloaded) == 20, "Helper-style reload changed initial read profile");
        for (const auto &version : {QString("d4-0-03"), QString("d4-3-01")}) {
            const QString ref = reference(sharedPath, version);
            check(RadioDefinition::loadProfile(ref, &radio, &codeplug, nullptr, &error), error);
            check(delay(radio) == (version == "d4-0-03" ? 21 : 22), "Wrong version override selected");
            check(RadioDefinition::loadProfile(radio.sourcePath(), &reloaded, &reloadedCodeplug, nullptr, &error), error);
            check(delay(reloaded) == delay(radio) && reloadedCodeplug.id() == codeplug.id(),
                  "Helper-style reload lost version selection");
        }
        // Both version IDs may share a root profile without changing the USB identity rules.
        const QString rootOnlyPath = package("root-only.wcp", [&] { auto m = versions; m["usb.json"] = shared; return m; }());
        check(RadioDefinition::loadProfile(reference(rootOnlyPath, "d4-3-01"), &radio, &codeplug, nullptr, &error), error);
        check(delay(radio) == 20 && radio.compatibleWithCodeplug(codeplug.document(), &error), error);
        auto unrelated = codeplug.document();
        unrelated["id"] = "unrelated";
        check(!radio.compatibleWithCodeplug(unrelated, &error), "Package binding accepted an unrelated codeplug");
        RadioDefinition unbound;
        check(RadioDefinition::fromJson(shared, &unbound, &error), error);
        check(!unbound.compatibleWithCodeplug(codeplug.document(), &error), "Unbound JSON ID checks were relaxed");
        QByteArray image;
        check(codeplug.decodedImage(codeplug.defaultCodeplug(), &image, &error), error);
        check(!RadioExecutor::planWrite(radio, codeplug.document(), RadioExecutor::Bytes(image.begin(), image.end())).empty(),
              "Shared profile could not plan a write for selected version");
        check(RadioDefinition::loadProfile(singlePath, &radio, &codeplug, nullptr, &error), error);
        check(codeplug.id() == "cs800", "Single-version package did not load root USB profile");
        const QString sentinel = radio.sourcePath();
        check(!RadioDefinition::loadProfile(reference(brokenPath, "d4-0-03"), &radio, &codeplug, nullptr, &error)
                  && error.contains(oldMember) && radio.sourcePath() == sentinel,
              "Invalid override fell back or replaced output");
        check(!RadioDefinition::loadProfile(brokenRootPath, &radio, &codeplug, nullptr, &error),
              "Invalid root fell back to standalone file");
        check(!RadioDefinition::loadProfile(reference(sharedPath, "missing"), &radio, &codeplug, nullptr, &error),
              "Unknown version fell back to default");
        check(!RadioDefinition::loadProfile(directory.filePath("version-only.wcp"), &radio, &codeplug, nullptr, &error),
              "Initial read used a version-specific profile");
        auto mismatch = singleRoot;
        mismatch["usb.json"] = profile(base, "unrelated", 20);
        check(!RadioDefinition::loadProfile(package("mismatch.wcp", mismatch), &radio, &codeplug, nullptr, &error),
              "Unrelated USB profile ID was accepted");

        check(CodeplugDefinitions::radioDefinitionSource("version-only.wcp").isEmpty(), "Initial picker offered version-only USB");
        check(!CodeplugDefinitions::radioDefinitionSource("version-only.wcp", "d4-0-03").isEmpty(), "Selected version lost its override");
        check(CodeplugDefinitions::radioDefinitionSource("version-only.wcp", "d4-3-01").isEmpty(), "Missing USB profile was available");
        check(CodeplugDefinitions::radioDefinitionSource("none.wcp").isEmpty(), "Missing USB profile was available");
        check(CodeplugDefinitions::radioDefinitionSource("legacy.wcp").isEmpty(), "Standalone file was still discovered");
        check(!RadioDefinition::loadProfile(directory.filePath("legacy.wrd"), &radio, &codeplug, nullptr, &error),
              "Standalone USB file was accepted");
        QStringList offered;
        for (const auto &option : CodeplugDefinitions::radioReadOptions())
            offered.append(option.toMap().value("fileName").toString());
        check(offered.contains("single.wcp") && offered.contains("shared #1.wcp") && !offered.contains("legacy.wcp")
                  && !offered.contains("version-only.wcp") && !offered.contains("none.wcp"),
              "Read picker did not follow initial-read profile availability");
        const QString brokenSource = CodeplugDefinitions::radioDefinitionSource("broken.wcp", "d4-0-03");
        check(brokenSource == reference(brokenPath, "d4-0-03"), "Catalog fell back around invalid override");
        qInfo("Root USB, version overrides, helper reloads, shared IDs, standalone-file rejection, and invalid-profile rejection passed");
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
