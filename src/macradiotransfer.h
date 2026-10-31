#pragma once

#include "radioexecutor.h"
#include <QString>

namespace MacRadioTransfer {
RadioExecutor::Bytes read(const QString &definitionPath, const RadioExecutor::Progress &progress,
                          const std::atomic_bool &cancelled);
void write(const QString &definitionPath, const RadioExecutor::Bytes &image,
           const RadioExecutor::Progress &progress);
}
