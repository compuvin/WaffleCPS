#pragma once

#include "codeplugreader.h"

class CodeplugWriter
{
public:
    static bool write(const QByteArray &source, const ImportedCodeplug &target,
                      const QString &radioName, int radioId, QByteArray *result,
                      QString *error);
};
