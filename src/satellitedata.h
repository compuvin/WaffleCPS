#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QVariantList>

namespace SatelliteData {
QByteArray encode(const QJsonObject &definition, const QVariantList &rows,
                  const QVariantList &orbits, QVariantList *writtenIds);
QByteArray merge(const QJsonObject &definition, const QByteArray &original,
                 const QByteArray &payload);
}
