#pragma once

#include "codeplugreader.h"

// Catalog of the definitions bundled with this build of Waffle.
class CodeplugDefinitions
{
public:
    static QString importFileFilter(QString *error);
    static bool read(const QByteArray &bytes, ImportedCodeplug *result, QString *error);
};
