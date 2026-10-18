#include "codeplugreader.h"

#include <QtEndian>

namespace {
constexpr qsizetype maskOffset = 0x300;
constexpr qsizetype channelNameOffset = 0x4368;
constexpr qsizetype channelRecordOffset = 0x23768;
constexpr qsizetype zoneOffset = 0x52954;
constexpr qsizetype contactOffset = 0x47d3c0;
constexpr int contactRecordLength = 16;
constexpr int maximumContacts = 10000;
constexpr int channelNameLength = 32;
constexpr int channelRecordLength = 48;
constexpr int zoneNameLength = 32;
constexpr int maximumChannels = 3000;
constexpr int maximumZones = 500;

QString utf16Name(const QByteArray &bytes, qsizetype offset, int length)
{
    QString name;
    for (int i = 0; i < length; i += 2) {
        const ushort character = qFromLittleEndian<quint16>(
            reinterpret_cast<const uchar *>(bytes.constData() + offset + i));
        if (character == 0)
            break;
        if (character < 0x20 || character == 0xffff)
            return {};
        name.append(QChar(character));
    }
    return name.trimmed();
}

quint32 u32(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

quint16 u16(const QByteArray &bytes, qsizetype offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

QString contactName(const QByteArray &bytes, qsizetype offset)
{
    // Eleven six-bit characters occupy 66 bits starting at record offset 8.
    // The last character uses the high two bits of the following record's first byte.
    QString result;
    for (int character = 0; character < 11; ++character) {
        const int bit = character * 6;
        const int byte = bit / 8;
        const int shift = bit % 8;
        const quint16 pair = (quint16(uchar(bytes[offset + byte])) << 8)
                             | uchar(bytes[offset + byte + 1]);
        const int code = (pair >> (10 - shift)) & 0x3f;
        if (code < 10)
            result.append(QChar('0' + code));
        else if (code < 36)
            result.append(QChar('A' + code - 10));
        else if (code < 62)
            result.append(QChar('a' + code - 36));
        else if (code == 62 || code == 63) // The sample uses 63 as a separator in private names.
            result.append(QChar(' '));
    }
    return result.trimmed();
}

bool plausibleFrequency(quint32 hz)
{
    return hz >= 100000000 && hz <= 999999999;
}
} // namespace

bool CodeplugReader::read(const QByteArray &bytes, ImportedCodeplug *result, QString *error)
{
    auto reject = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!result || bytes.size() < zoneOffset + 36 || bytes.size() < maskOffset + 256)
        return reject(QStringLiteral("File is too small for a CS7000-M17 Plus codeplug."));
    if (bytes.mid(maskOffset, 256) != bytes.mid(maskOffset + 256, 256))
        return reject(QStringLiteral("Codeplug encoding does not match the supported layout."));

    QByteArray decoded(bytes.size(), Qt::Uninitialized);
    const char *mask = bytes.constData() + maskOffset;
    for (qsizetype i = 0; i < bytes.size(); ++i)
        decoded[i] = bytes[i] ^ mask[i % 256];

    if (decoded.mid(0x1200, 16) != QByteArray("CS7000-M17 PLUS\0", 16))
        return reject(QStringLiteral("This is not a CS7000-M17 Plus codeplug."));
    if (decoded.mid(0x1298, 12) != QByteArray("V1.02.20.00\0", 12))
        return reject(QStringLiteral("This CPS version has not been verified for import."));

    ImportedCodeplug parsed;
    parsed.model = QStringLiteral("CS7000-M17 PLUS");
    parsed.cpsVersion = QStringLiteral("V1.02.20.00");
    for (int index = 0; index < maximumChannels; ++index) {
        const qsizetype nameAt = channelNameOffset + index * channelNameLength;
        const qsizetype recordAt = channelRecordOffset + index * channelRecordLength;
        if (nameAt + channelNameLength > decoded.size()
            || recordAt + channelRecordLength > decoded.size())
            return reject(QStringLiteral("Channel table is truncated."));
        const QString name = utf16Name(decoded, nameAt, channelNameLength);
        if (name.isEmpty())
            break;
        const quint32 frequencyA = u32(decoded, recordAt);
        const quint32 frequencyB = u32(decoded, recordAt + 4);
        if (!plausibleFrequency(frequencyA) || !plausibleFrequency(frequencyB))
            return reject(QStringLiteral("Channel %1 has invalid frequencies.").arg(index + 1));
        const uchar modeMarker = static_cast<uchar>(decoded[recordAt + 23]);
        const uchar digitalSettings = static_cast<uchar>(decoded[recordAt + 16]);
        const bool digital = modeMarker == 0x20 || modeMarker == 0x41;
        const int contactIndex = digital ? u16(decoded, recordAt + 24) : 0;
        parsed.channels.append({name, frequencyA, frequencyB, digital,
                                digital ? int(digitalSettings & 0x0f) : 0,
                                digital ? (digitalSettings & 0x10 ? 2 : 1) : 0,
                                contactIndex});
    }
    if (parsed.channels.isEmpty())
        return reject(QStringLiteral("No channels were found at the expected location."));

    qsizetype cursor = zoneOffset;
    for (int index = 0; index < maximumZones; ++index) {
        if (cursor + zoneNameLength + 4 > decoded.size())
            return reject(QStringLiteral("Zone table is truncated."));
        const QString name = utf16Name(decoded, cursor, zoneNameLength);
        if (name.isEmpty())
            break;
        const quint32 count = u32(decoded, cursor + zoneNameLength);
        if (count > 16 || cursor + 36 + 2 * count > decoded.size())
            return reject(QStringLiteral("Zone %1 has an invalid member count.").arg(index + 1));
        ImportedZone zone;
        zone.name = name;
        for (quint32 member = 0; member < count; ++member) {
            const quint16 channelNumber = u16(decoded, cursor + 36 + 2 * member);
            if (channelNumber == 0 || channelNumber > parsed.channels.size())
                return reject(QStringLiteral("Zone %1 references an unknown channel.").arg(index + 1));
            zone.channelIndexes.append(channelNumber - 1); // Radio indexes are one-based.
        }
        parsed.zones.append(std::move(zone));
        cursor += 36 + 2 * count;
    }
    if (parsed.zones.isEmpty())
        return reject(QStringLiteral("No zones were found at the expected location."));
    if (decoded.size() >= contactOffset + 2 * contactRecordLength) {
        for (int index = 1; index <= maximumContacts; ++index) {
            const qsizetype record = contactOffset + index * contactRecordLength;
            if (record + contactRecordLength + 1 > decoded.size())
                break;
            const int id = uchar(decoded[record + 5])
                           | (int(uchar(decoded[record + 6])) << 8)
                           | (int(uchar(decoded[record + 7])) << 16);
            if (id == 0)
                break;
            const QString name = contactName(decoded, record + 8);
            if (name.isEmpty() || uchar(decoded[record + 4]) > 1)
                return reject(QStringLiteral("Contact %1 has invalid details.").arg(index));
            parsed.contacts.append({name, id, uchar(decoded[record + 4]) == 1});
        }
    }
    for (const auto &channel : parsed.channels) {
        if (channel.contactIndex > parsed.contacts.size())
            return reject(QStringLiteral("A channel references an unknown contact."));
    }
    *result = std::move(parsed);
    return true;
}
