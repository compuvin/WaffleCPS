#include "radiocodeplug.h"
#include "codeplugreader.h"
#include "cs7000default.h"

bool radioCodeplugFromRaw(const QByteArray &raw, QByteArray *encoded, QString *error)
{
    auto reject = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    if (!encoded || raw.size() < 0x500)
        return reject(QStringLiteral("The downloaded codeplug is truncated."));
    // Our supported .rdbx encoding stores its mask in these decoded-zero regions.
    // Refuse an incompatible layout rather than overwrite any downloaded settings.
    if (raw.mid(0x300, 0x200) != QByteArray(0x200, '\0'))
        return reject(QStringLiteral("The radio image has an unsupported encoding layout."));
    const QByteArray mask = cs7000DefaultCodeplug().mid(0x300, 256);
    if (mask.size() != 256)
        return reject(QStringLiteral("The embedded codeplug encoding mask is unavailable."));
    QByteArray output(raw.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < raw.size(); ++i)
        output[i] = raw[i] ^ mask[i % 256];
    ImportedCodeplug checked;
    QString readError;
    if (!CodeplugReader::read(output, &checked, &readError))
        return reject(QStringLiteral("Could not decode the downloaded codeplug: %1").arg(readError));
    *encoded = std::move(output);
    return true;
}
