#pragma once
#include <QDate>
#include <QMap>
#include <QPair>
#include <QRegularExpression>
#include <QString>
#include <cmath>

namespace SatelliteOrbits {
using Records = QMap<int, QPair<QString, QString>>;
inline bool parse(const QByteArray &bytes, Records *result, QString *error)
{
    Records records;
    auto reject = [&](const QString &message) { *error = message; return false; };
    const auto lines = QString::fromLatin1(bytes).split('\n');
    QString first;
    for (int i = 0; i < lines.size(); ++i) {
        QString line = lines[i];
        if (line.endsWith('\r')) line.chop(1);
        if (line.trimmed().isEmpty()) continue;
        if (!line.startsWith("1 ") && !line.startsWith("2 ")) {
            if (!first.isEmpty()) return reject(QStringLiteral("Missing TLE line 2 before line %1.").arg(i + 1));
            if (line.contains('<') || line.contains('>')) return reject(QStringLiteral("The download contains HTML instead of TLE data."));
            continue; // Optional satellite name preceding a pair.
        }
        if (line.size() != 69)
            return reject(QStringLiteral("TLE line %1 must contain 69 characters.").arg(i + 1));
        int checksum = 0;
        for (int at = 0; at < 68; ++at) {
            const auto ch = line[at];
            if (ch >= '0' && ch <= '9') checksum += ch.unicode() - '0';
            else if (ch == '-') ++checksum;
        }
        if (line[68] < '0' || line[68] > '9' || checksum % 10 != line[68].unicode() - '0')
            return reject(QStringLiteral("TLE checksum failed at line %1.").arg(i + 1));
        const QString idText = line.mid(2, 5).trimmed();
        if (!QRegularExpression("^[0-9]{1,5}$").match(idText).hasMatch() || idText.toInt() <= 0)
            return reject(QStringLiteral("Unsupported catalog ID at TLE line %1.").arg(i + 1));
        if (line.startsWith("1 ")) {
            if (!first.isEmpty()) return reject(QStringLiteral("A TLE line 2 is missing."));
            if (!QRegularExpression("^[0-9]{2}[0-9]{3}\\.[0-9]{8}$").match(line.mid(18, 14)).hasMatch()
                || !QRegularExpression("^[ +\\-]\\.[0-9]{8}$").match(line.mid(33, 10)).hasMatch())
                return reject(QStringLiteral("Invalid epoch or motion derivative at TLE line %1.").arg(i + 1));
            const int yearValue = line.mid(18, 2).toInt();
            const int year = yearValue < 57 ? 2000 + yearValue : 1900 + yearValue;
            const double day = line.mid(20, 12).toDouble();
            if (day < 1 || day >= QDate(year, 12, 31).dayOfYear() + 1)
                return reject(QStringLiteral("Invalid TLE epoch day."));
            first = line;
            continue;
        }
        const int id = idText.toInt();
        if (first.isEmpty() || first.mid(2, 5).trimmed().toInt() != id)
            return reject(QStringLiteral("TLE line pair has mismatched catalog IDs."));
        for (int offset : {8, 17, 34, 43, 52}) {
            const int length = offset == 52 ? 11 : 8;
            const QString valueText = line.mid(offset, length).trimmed();
            bool ok = false;
            const double value = valueText.toDouble(&ok);
            if (!ok || !std::isfinite(value)
                || !QRegularExpression("^[0-9]+\\.[0-9]+$").match(valueText).hasMatch()
                || value < 0 || (offset == 8 && value > 180)
                || ((offset == 17 || offset == 34 || offset == 43) && value >= 360)
                || (offset == 52 && value <= 0))
                return reject(QStringLiteral("Invalid orbital field at TLE line %1.").arg(i + 1));
        }
        if (!QRegularExpression("^[0-9]{7}$").match(line.mid(26, 7)).hasMatch()
            || !QRegularExpression("^[0-9]+$").match(line.mid(63, 5).trimmed()).hasMatch())
            return reject(QStringLiteral("Invalid eccentricity or revolution number."));
        const auto pair = qMakePair(first, line);
        if (records.contains(id) && records.value(id) != pair)
            return reject(QStringLiteral("Conflicting TLE records for catalog ID %1.").arg(id));
        records.insert(id, pair);
        first.clear();
    }
    if (!first.isEmpty()) return reject(QStringLiteral("The download ends before TLE line 2."));
    if (records.isEmpty()) return reject(QStringLiteral("The download contains no valid TLE records."));
    *result = records;
    error->clear();
    return true;
}
}
