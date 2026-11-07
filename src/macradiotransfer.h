#pragma once

#include "radioexecutor.h"
#include <QString>

namespace MacRadioTransfer {
RadioExecutor::Bytes read(const RadioDefinition &definition, const QString &definitionPath,
                          const RadioExecutor::Progress &progress,
                          const std::atomic_bool &cancelled);
void write(const RadioDefinition &definition, const QString &definitionPath,
           const QJsonObject &codeplugDefinition, const RadioExecutor::Bytes &image,
           const RadioExecutor::Progress &progress);
}
