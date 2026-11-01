#pragma once

#include "radioexecutor.h"

namespace DfuExecutor {
RadioExecutor::Bytes read(const RadioDefinition &definition,
                          const RadioExecutor::Progress &progress,
                          const std::atomic_bool &cancelled);
void write(const RadioDefinition &definition, const RadioExecutor::Bytes &image,
           const RadioExecutor::Progress &progress);
RadioExecutor::Bytes writePayload(const RadioDefinition &definition,
                                   const RadioExecutor::Bytes &image);
}
