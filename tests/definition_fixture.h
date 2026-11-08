#pragma once

#include "../src/codeplugdefinition.h"
#include "../src/ziparchive.h"
#include <QDir>

#include <QCoreApplication>
#include <QFileInfo>
#include <stdexcept>

inline const CodeplugDefinition &testCodeplugDefinition()
{
    static const CodeplugDefinition definition = [] {
        CodeplugDefinition loaded;
        QString error;
        const QFileInfo header(QString::fromUtf8(__FILE__));
        const QString root = header.isAbsolute() ? header.absolutePath() + QStringLiteral("/..")
            : QCoreApplication::applicationDirPath() + QStringLiteral("/..");
        const QString path = root + QStringLiteral("/definitions/cs7000-m17-plus.wcp");
        if (!CodeplugDefinition::load(path, &loaded, &error))
            throw std::runtime_error(error.toStdString());
        return loaded;
    }();
    return definition;
}

inline QByteArray packagedTestCodeplug()
{
    return testCodeplugDefinition().defaultCodeplug();
}

inline bool readTestCodeplug(const QByteArray &bytes, ImportedCodeplug *result, QString *error)
{
    return testCodeplugDefinition().read(bytes, result, error);
}

inline bool writeTestCodeplug(const QByteArray &source, const ImportedCodeplug &target,
                              const QString &name, int id, QByteArray *result, QString *error)
{
    return testCodeplugDefinition().write(source, target, name, id, result, error);
}

// Keep disabled-transfer checks independent of the radios that gain USB support.
inline bool definitionsWithoutUsb(const QString &destination, const QString &radio, QString *error)
{
    const QFileInfo header(QString::fromUtf8(__FILE__));
    const QString root = header.isAbsolute() ? header.absolutePath() + "/.."
        : QCoreApplication::applicationDirPath() + "/..";
    const QDir source(qEnvironmentVariable("WAFFLE_DEFINITIONS_DIR", root + "/definitions"));
    const auto files = source.entryList({"*.wcp"}, QDir::Files);
    if (!files.contains(radio)) { *error = "Missing disabled-transfer fixture source"; return false; }
    for (const auto &name : files) {
        QMap<QString, QByteArray> members;
        if (!ZipArchive::read(source.filePath(name), &members, error)) return false;
        if (name == radio) {
            for (const auto &member : members.keys())
                if (member == "usb.json" || member.endsWith("/usb.json")) members.remove(member);
        }
        if (!ZipArchive::write(QDir(destination).filePath(name), members, error)) return false;
    }
    qputenv("WAFFLE_DEFINITIONS_DIR", destination.toUtf8());
    return true;
}
