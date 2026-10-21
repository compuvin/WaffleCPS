#include "codeplugwriter.h"

#include <QtEndian>
#include <algorithm>

namespace {
constexpr qsizetype channelCountAt = 0x2424;
constexpr qsizetype channelIndexAt = 0x2428;
constexpr qsizetype channelNameAt = 0x4368;
constexpr qsizetype channelRecordAt = 0x23768;
constexpr qsizetype zoneCountAt = 0x52568;
constexpr qsizetype zoneIndexAt = 0x52570;
constexpr qsizetype zoneTableAt = 0x52954;
constexpr qsizetype contactPointerAt = 0x1264;
constexpr qsizetype contactTableAfterCount = 0x3f79f3;
constexpr qsizetype radioIdFromEnd = 0xc10c93;

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

quint32 read32(const QByteArray &bytes, qsizetype at)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + at));
}

void write16(QByteArray *bytes, qsizetype at, quint16 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes->data() + at));
}

void write32(QByteArray *bytes, qsizetype at, quint32 value)
{
    qToLittleEndian(value, reinterpret_cast<uchar *>(bytes->data() + at));
}

void append16(QByteArray *bytes, quint16 value)
{
    bytes->append(char(value & 0xff));
    bytes->append(char(value >> 8));
}

void append32(QByteArray *bytes, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes->append(char(value >> shift));
}

bool fitsUtf16(const QString &name, int maximum)
{
    if (name.isEmpty() || name.size() > maximum)
        return false;
    for (QChar character : name)
        if (character.unicode() < 0x20 || character.unicode() == 0xffff)
            return false;
    return true;
}

void writeName(QByteArray *bytes, qsizetype at, int capacity, const QString &name)
{
    std::fill(bytes->begin() + at, bytes->begin() + at + capacity, char(0));
    for (int i = 0; i < name.size(); ++i)
        write16(bytes, at + i * 2, name.at(i).unicode());
}

bool fitsContactName(const QString &name)
{
    if (name.isEmpty() || name.size() > 16)
        return false;
    for (QChar character : name)
        if (!(character >= QChar('0') && character <= QChar('9'))
            && !(character >= QChar('A') && character <= QChar('Z'))
            && !(character >= QChar('a') && character <= QChar('z'))
            && character != QChar(' '))
            return false;
    return true;
}

int contactCode(QChar character, bool padding)
{
    if (character >= QChar('0') && character <= QChar('9'))
        return character.unicode() - '0';
    if (character >= QChar('A') && character <= QChar('Z'))
        return character.unicode() - 'A' + 10;
    if (character >= QChar('a') && character <= QChar('z'))
        return character.unicode() - 'a' + 36;
    return padding ? 62 : 63;
}

void writeContactName(QByteArray *bytes, qsizetype at, const QString &name)
{
    // Sixteen characters extend through the next record's unused prefix.
    for (int character = 0; character < 16; ++character) {
        const int code = contactCode(character < name.size() ? name.at(character) : QChar(' '),
                                     character >= name.size());
        for (int part = 0; part < 6; ++part) {
            const int bit = character * 6 + part;
            const qsizetype byteAt = at + 8 + bit / 8;
            const uchar mask = uchar(1 << (7 - bit % 8));
            const uchar old = uchar((*bytes)[byteAt]);
            (*bytes)[byteAt] = char((old & ~mask) | ((code & (1 << (5 - part))) ? mask : 0));
        }
    }
}

bool sameData(const ImportedCodeplug &a, const ImportedCodeplug &b)
{
    if (a.channels.size() != b.channels.size() || a.contacts.size() != b.contacts.size()
        || a.zones.size() != b.zones.size())
        return false;
    for (int i = 0; i < a.channels.size(); ++i) {
        const auto &x = a.channels.at(i);
        const auto &y = b.channels.at(i);
        if (x.name != y.name || x.frequencyAHz != y.frequencyAHz
            || x.frequencyBHz != y.frequencyBHz || x.digital != y.digital
            || x.colorCode != y.colorCode || x.timeSlot != y.timeSlot
            || x.contactIndex != y.contactIndex
            || (x.highPower.has_value() && x.highPower != y.highPower))
            return false;
    }
    for (int i = 0; i < a.contacts.size(); ++i) {
        const auto &x = a.contacts.at(i);
        const auto &y = b.contacts.at(i);
        if (x.name != y.name || x.id != y.id || x.groupCall != y.groupCall)
            return false;
    }
    for (int i = 0; i < a.zones.size(); ++i)
        if (a.zones.at(i).name != b.zones.at(i).name
            || a.zones.at(i).channelIndexes != b.zones.at(i).channelIndexes)
            return false;
    return true;
}
} // namespace

bool CodeplugWriter::write(const QByteArray &source, const ImportedCodeplug &target,
                           const QString &radioName, int radioId, QByteArray *result,
                           QString *error)
{
    if (!result)
        return fail(error, QStringLiteral("No export output was supplied."));
    ImportedCodeplug original;
    if (!CodeplugReader::read(source, &original, error))
        return false;
    if (target.model != original.model || target.cpsVersion != original.cpsVersion)
        return fail(error, QStringLiteral("The codeplug does not match its export base."));
    if (!fitsUtf16(radioName, 16) || radioId < 1000000 || radioId > 9999999)
        return fail(error, QStringLiteral("Enter a radio name of up to 16 characters and a seven-digit DMR ID."));
    if (target.channels.isEmpty() || target.channels.size() > 3000
        || target.zones.isEmpty() || target.zones.size() > 250
        || target.contacts.size() > 10000)
        return fail(error, QStringLiteral("The channel, zone, or contact count exceeds a CS7000 limit."));
    for (const auto &channel : target.channels) {
        if (!fitsUtf16(channel.name, 16) || channel.frequencyAHz < 100000000
            || channel.frequencyAHz > 999999999 || channel.frequencyBHz < 100000000
            || channel.frequencyBHz > 999999999 || channel.colorCode < 0
            || channel.colorCode > 15 || channel.contactIndex < 0
            || channel.contactIndex > target.contacts.size()
            || (channel.digital && (channel.timeSlot < 1 || channel.timeSlot > 2)))
            return fail(error, QStringLiteral("A channel does not fit the CS7000 record format."));
    }
    for (const auto &contact : target.contacts)
        if (!fitsContactName(contact.name) || contact.id < 1 || contact.id > 0xffffff)
            return fail(error, QStringLiteral("A contact needs a radio name of up to 16 characters and a valid ID."));
    for (const auto &zone : target.zones) {
        if (!fitsUtf16(zone.name, 16) || zone.channelIndexes.size() > 16)
            return fail(error, QStringLiteral("A zone name or channel count exceeds the CS7000 limit."));
        for (int index : zone.channelIndexes)
            if (index < 0 || index >= target.channels.size())
                return fail(error, QStringLiteral("A zone references an unknown channel."));
    }
    if (sameData(target, original) && radioName == original.radioName && radioId == original.radioId) {
        *result = source;
        return true;
    }

    const QByteArray mask = source.mid(0x300, 256);
    QByteArray decoded(source.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < source.size(); ++i)
        decoded[i] = source[i] ^ mask[i % 256];
    const QByteArray originalDecoded = decoded;
    writeName(&decoded, 0x1300, 32, radioName);
    const qsizetype idAt = decoded.size() - radioIdFromEnd;
    if (idAt < 0 || idAt + 5 > decoded.size())
        return fail(error, QStringLiteral("The radio ID record is missing."));
    write32(&decoded, idAt, quint32(radioId));
    decoded[idAt + 4] = char(0x09); // Both controlled seven-digit ID samples use 0x09.
    if (sameData(target, original)) {
        QByteArray output(decoded.size(), Qt::Uninitialized);
        for (qsizetype i = 0; i < decoded.size(); ++i)
            output[i] = decoded[i] ^ mask[i % 256];
        ImportedCodeplug checked;
        QString checkError;
        if (!CodeplugReader::read(output, &checked, &checkError)
            || checked.radioName != radioName || checked.radioId != radioId)
            return fail(error, QStringLiteral("The exported radio settings failed their import check: %1").arg(checkError));
        *result = std::move(output);
        return true;
    }

    write32(&decoded, channelCountAt, quint32(target.channels.size()));
    for (int i = 0; i < std::max(original.channels.size(), target.channels.size()); ++i) {
        write16(&decoded, channelIndexAt + 2 * i, i < target.channels.size() ? quint16(i + 1) : 0);
        const qsizetype nameAt = channelNameAt + 32 * i;
        const qsizetype recordAt = channelRecordAt + 48 * i;
        if (i >= target.channels.size()) {
            std::fill(decoded.begin() + nameAt, decoded.begin() + nameAt + 32, char(0));
            std::fill(decoded.begin() + recordAt, decoded.begin() + recordAt + 48, char(0));
            continue;
        }
        const auto &channel = target.channels.at(i);
        int sourceIndex = -1;
        if (i < original.channels.size() && original.channels.at(i).name == channel.name
            && original.channels.at(i).digital == channel.digital)
            sourceIndex = i;
        if (sourceIndex < 0)
            for (int j = 0; j < original.channels.size(); ++j)
                if (original.channels.at(j).name == channel.name
                    && original.channels.at(j).digital == channel.digital) {
                    sourceIndex = j;
                    break;
                }
        if (sourceIndex < 0)
            for (int j = 0; j < original.channels.size(); ++j)
                if (original.channels.at(j).digital == channel.digital) {
                    sourceIndex = j;
                    break;
                }
        if (sourceIndex < 0)
            return fail(error, QStringLiteral("The export base has no matching channel mode."));
        const QByteArray record = originalDecoded.mid(channelRecordAt + 48 * sourceIndex, 48);
        std::copy(record.begin(), record.end(), decoded.begin() + recordAt);
        writeName(&decoded, nameAt, 32, channel.name);
        write32(&decoded, recordAt, channel.frequencyAHz);
        write32(&decoded, recordAt + 4, channel.frequencyBHz);
        if (channel.highPower.has_value())
            decoded[recordAt + 9] = char((uchar(decoded[recordAt + 9]) & ~0x80)
                                         | (*channel.highPower ? 0x80 : 0));
        if (channel.digital) {
            decoded[recordAt + 16] = char((uchar(decoded[recordAt + 16]) & ~0x1f)
                                           | channel.colorCode | (channel.timeSlot == 2 ? 0x10 : 0));
            write16(&decoded, recordAt + 24, quint16(channel.contactIndex));
        }
    }

    QByteArray zoneRecords;
    for (const auto &zone : target.zones) {
        const qsizetype at = zoneRecords.size();
        zoneRecords.append(QByteArray(32, '\0'));
        writeName(&zoneRecords, at, 32, zone.name);
        append32(&zoneRecords, quint32(zone.channelIndexes.size()));
        for (int index : zone.channelIndexes)
            append16(&zoneRecords, quint16(index + 1));
    }
    qsizetype oldZoneBytes = 0;
    for (const auto &zone : original.zones)
        oldZoneBytes += 36 + 2 * zone.channelIndexes.size();
    const qsizetype difference = zoneRecords.size() - oldZoneBytes;
    const quint32 oldContactPointer = read32(decoded, contactPointerAt);
    if (oldContactPointer < zoneTableAt + oldZoneBytes
        || qint64(oldContactPointer) + difference <= 0)
        return fail(error, QStringLiteral("The contact pointer is invalid."));
    decoded.replace(zoneTableAt, oldZoneBytes, zoneRecords);
    // Relocate sections following the variable-length zone table.
    for (qsizetype at = 0xfc; at <= 0x248; at += 4) {
        const quint32 offset = read32(originalDecoded, at);
        if (offset == 0)
            continue;
        const qint64 relocated = qint64(offset) + difference;
        if (offset < zoneTableAt + oldZoneBytes
            || relocated < 0 || relocated >= decoded.size())
            return fail(error, QStringLiteral("A relocated section offset is invalid."));
        write32(&decoded, at, quint32(relocated));
    }
    write32(&decoded, contactPointerAt, quint32(qint64(oldContactPointer) + difference));
    write32(&decoded, zoneCountAt, quint32(target.zones.size()));
    for (int i = 0; i < std::max(original.zones.size(), target.zones.size()) - 1; ++i)
        write32(&decoded, zoneIndexAt + 4 * i, 0);
    quint32 start = 0;
    for (int i = 0; i < target.zones.size(); ++i) {
        if (i > 0)
            write32(&decoded, zoneIndexAt + 4 * (i - 1), start);
        start += 36 + 2 * target.zones.at(i).channelIndexes.size();
    }

    const qsizetype countAt = read32(decoded, contactPointerAt);
    const qsizetype tableAt = countAt + contactTableAfterCount;
    const qsizetype firstRecord = tableAt + 16;
    if (tableAt < 0 || countAt + 8 + 4 * std::max(original.contacts.size(), target.contacts.size()) > tableAt
        || firstRecord + 16 * (target.contacts.size() + 1) > decoded.size())
        return fail(error, QStringLiteral("The contact table has insufficient space."));
    write32(&decoded, countAt, quint32(target.contacts.size()));
    for (int i = 0; i < std::max(original.contacts.size(), target.contacts.size()); ++i) {
        write32(&decoded, countAt + 8 + 4 * i,
                i < target.contacts.size() ? quint32(i + 1) : 0);
        const qsizetype at = firstRecord + 16 * i;
        if (i >= target.contacts.size()) {
            std::fill(decoded.begin() + at, decoded.begin() + at + 16, char(0));
            continue;
        }
        if (i >= original.contacts.size())
            std::fill(decoded.begin() + at, decoded.begin() + at + 16, char(0));
        const auto &contact = target.contacts.at(i);
        decoded[at + 5] = char(contact.id & 0xff);
        decoded[at + 6] = char((contact.id >> 8) & 0xff);
        decoded[at + 7] = char((contact.id >> 16) & 0xff);
        std::fill(decoded.begin() + at + 8, decoded.begin() + at + 16, char(0));
    }
    for (int i = 0; i < target.contacts.size(); ++i) {
        writeContactName(&decoded, firstRecord + 16 * i, target.contacts.at(i).name);
        decoded[firstRecord + 16 * i + 20] =
            target.contacts.at(i).groupCall ? char(1) : char(0);
    }

    QByteArray output(decoded.size(), Qt::Uninitialized);
    for (qsizetype i = 0; i < decoded.size(); ++i)
        output[i] = decoded[i] ^ mask[i % 256];
    ImportedCodeplug checked;
    QString checkError;
    if (!CodeplugReader::read(output, &checked, &checkError))
        return fail(error, QStringLiteral("The exported codeplug failed its import check: %1").arg(checkError));
    if (checked.radioName != radioName || checked.radioId != radioId)
        return fail(error, QStringLiteral("The exported radio identity did not reimport correctly."));
    if (!sameData(target, checked))
        return fail(error, QStringLiteral("Exported records differ after reimport: %1/%2 channels, %3/%4 contacts, %5/%6 zones.")
                    .arg(checked.channels.size()).arg(target.channels.size())
                    .arg(checked.contacts.size()).arg(target.contacts.size())
                    .arg(checked.zones.size()).arg(target.zones.size()));
    *result = std::move(output);
    return true;
}
