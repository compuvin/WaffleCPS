#pragma once

#include "codeplugreader.h"
#include <QJsonObject>
#include <QStringList>

// Definition-driven file handling. No Python or USB dependency.
class CodeplugDefinition
{
public:
    static bool load(const QString &path, CodeplugDefinition *result, QString *error);

    QString id() const;
    QString modelName() const;
    QStringList extensions() const;
    bool exportExtension(const QByteArray &source, QString *extension, QString *error) const;
    QString diskProvenance() const;
    QByteArray defaultCodeplug() const;
    QByteArray iconPng() const;
    bool defaultForNew() const;

    // Model recognition deliberately does not imply structural validation.
    bool matchesModel(const QByteArray &input, QString *error) const;
    // Decode and validate declared binary fields/tables independently of editor bindings.
    bool inspect(const QByteArray &input, QJsonObject *result, QString *error) const;
    bool read(const QByteArray &input, ImportedCodeplug *result, QString *error) const;
    bool write(const QByteArray &source, const ImportedCodeplug &target,
               const QString &radioName, int radioId,
               QByteArray *result, QString *error) const;

private:
    QJsonObject m_definition;
    QByteArray m_defaultCodeplug;
    QByteArray m_iconPng;
    QString m_diskPath;
    QByteArray m_definitionSha256;
};
