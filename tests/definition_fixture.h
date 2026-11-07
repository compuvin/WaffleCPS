#pragma once

#include "../src/codeplugdefinition.h"

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
