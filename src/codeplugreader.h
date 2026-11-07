#pragma once

#include "importedcodeplug.h"
#include <QByteArray>

class CodeplugReader
{
public:
    // Reads the observed CS7000-M17 Plus CPS V1.02.20.00 and V1.02.21.00 layouts.
    static bool read(const QByteArray &bytes, ImportedCodeplug *result, QString *error);
};
