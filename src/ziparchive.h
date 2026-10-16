#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

namespace ZipArchive {

// A small ZIP32 reader/writer for WaffleCPS project files. Entries are UTF-8,
// deflated, and held in memory; the project format has a 256 MiB entry limit.
bool write(const QString &path, const QMap<QString, QByteArray> &entries, QString *error);
bool read(const QString &path, QMap<QString, QByteArray> *entries, QString *error);

} // namespace ZipArchive
