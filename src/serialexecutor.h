#pragma once

#include "radioexecutor.h"

namespace SerialExecutor {
void writeSatellites(const RadioDefinition &definition, const QJsonObject &satellites,
                     const QByteArray &payload, const RadioExecutor::Progress &progress);
bool validate(const QJsonObject &definition, QString *error);
RadioExecutor::Bytes read(const RadioDefinition &definition,
                          const RadioExecutor::Progress &progress,
                          const std::atomic_bool &cancelled);
void write(const RadioDefinition &definition, const RadioExecutor::Bytes &image,
           const RadioExecutor::Progress &progress);
}
