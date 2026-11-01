#pragma once

#include "radiodefinition.h"
#include <QByteArray>

QByteArray assembleRadioImage(const RadioDefinition &definition,
                              const QByteArray &codeplugTemplate,
                              const QByteArray &downloaded);
