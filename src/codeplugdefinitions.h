#pragma once

#include "importedcodeplug.h"
#include "codeplugdefinition.h"
#include <QByteArray>
#include <QVariantList>

struct RadioProfileData {
    CodeplugDefinition codeplug;
    QByteArray json;
    QString packagePath;
    QString reference;
    QString description;
    QStringList companionIds;
};

// Catalog loaded from the definitions folder on disk at first use.
class CodeplugDefinitions
{
public:
    static QString importFileFilter(QString *error, QString *warnings = nullptr);
    static QString definitionForFilter(const QString &filter, QString *error);
    static QString diskLoadReport();
    static QVariantList newProjectOptions();
    static QVariantList radioReadOptions();
    static QByteArray iconPng(const QString &definitionFile);
    static QString modelName(const QString &definitionFile);
    static QString radioDefinitionSource(const QString &definitionFile, const QString &version = {});
    static bool loadRadioProfile(const QString &source, RadioProfileData *result, QString *error);
    static QString defaultDefinitionFile();
    static QString exportFileFilter(const QByteArray &source,
                                    QString *extension, QString *error,
                                    const QString &definitionFile = {});
    static bool read(const QByteArray &bytes, ImportedCodeplug *result, QString *error,
                     const QString &definitionFile = {});
    static bool encodeRadioImage(const QByteArray &decoded, const QString &definitionFile,
                                 QByteArray *encoded, QString *extension, QString *error);
    static QByteArray defaultCodeplug(QString *error);
    static QByteArray defaultCodeplug(const QString &definitionFile, QString *error);
    static bool write(const QByteArray &source, const ImportedCodeplug &target,
                      const QString &radioName, int radioId,
                      QByteArray *result, QString *error);
};
