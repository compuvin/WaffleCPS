#pragma once

#include "codeplugreader.h"
#include <QVariantList>

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
    static bool hasCompanionWrd(const QString &definitionFile);
    static QString companionWrdPath(const QString &definitionFile);
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
