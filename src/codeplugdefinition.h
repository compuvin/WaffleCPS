#pragma once

#include "codeplugreader.h"
#include <QJsonObject>
#include <QStringList>

// Read-only first stage of the definition engine. No Python or USB dependency.
class CodeplugDefinition
{
public:
    static bool load(const QString &path, CodeplugDefinition *result, QString *error);

    QString id() const;
    QString modelName() const;
    QStringList extensions() const;
    QByteArray defaultCodeplug() const;

    // Model recognition deliberately does not imply structural validation.
    bool matchesModel(const QByteArray &input, QString *error) const;
    bool read(const QByteArray &input, ImportedCodeplug *result, QString *error) const;

private:
    QJsonObject m_definition;
    QByteArray m_defaultCodeplug;
};
