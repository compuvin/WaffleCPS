#include "ziparchive.h"

#include <QFile>
#include <QSaveFile>
#include <limits>
#include <zlib.h>

namespace {

constexpr quint32 localSignature = 0x04034b50;
constexpr quint32 centralSignature = 0x02014b50;
constexpr quint32 endSignature = 0x06054b50;
constexpr quint16 utf8Flag = 0x0800;
constexpr quint16 deflateMethod = 8;
constexpr quint16 storedMethod = 0;
constexpr quint32 maximumEntrySize = 256u * 1024u * 1024u;
constexpr qint64 maximumArchiveSize = 512ll * 1024ll * 1024ll;

void append16(QByteArray &bytes, quint16 value)
{
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
}

void append32(QByteArray &bytes, quint32 value)
{
    append16(bytes, static_cast<quint16>(value & 0xffff));
    append16(bytes, static_cast<quint16>(value >> 16));
}

bool read16(const QByteArray &bytes, qint64 offset, quint16 *value)
{
    if (offset < 0 || offset + 2 > bytes.size())
        return false;
    *value = static_cast<quint8>(bytes.at(offset))
             | (static_cast<quint16>(static_cast<quint8>(bytes.at(offset + 1))) << 8);
    return true;
}

bool read32(const QByteArray &bytes, qint64 offset, quint32 *value)
{
    quint16 low = 0;
    quint16 high = 0;
    if (!read16(bytes, offset, &low) || !read16(bytes, offset + 2, &high))
        return false;
    *value = low | (static_cast<quint32>(high) << 16);
    return true;
}

quint32 checksum(const QByteArray &bytes)
{
    return static_cast<quint32>(crc32(0, reinterpret_cast<const Bytef *>(bytes.constData()),
                                      static_cast<uInt>(bytes.size())));
}

bool compress(const QByteArray &input, QByteArray *output)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS,
                     MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;

    output->resize(static_cast<qsizetype>(compressBound(static_cast<uLong>(input.size()))));
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output->data());
    stream.avail_out = static_cast<uInt>(output->size());
    const int result = deflate(&stream, Z_FINISH);
    const bool success = result == Z_STREAM_END;
    if (success)
        output->resize(static_cast<qsizetype>(stream.total_out));
    deflateEnd(&stream);
    return success;
}

bool decompress(const QByteArray &input, quint32 expectedSize, QByteArray *output)
{
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
        return false;

    output->resize(expectedSize == 0 ? 1 : expectedSize);
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef *>(output->data());
    stream.avail_out = static_cast<uInt>(output->size());
    const int result = inflate(&stream, Z_FINISH);
    const bool success = result == Z_STREAM_END && stream.total_out == expectedSize
                         && stream.total_in == static_cast<uLong>(input.size());
    inflateEnd(&stream);
    if (success)
        output->resize(expectedSize);
    return success;
}

struct EntryInfo {
    QByteArray name;
    quint16 method = 0;
    quint32 checksum = 0;
    quint32 compressedSize = 0;
    quint32 uncompressedSize = 0;
    quint32 localOffset = 0;
};

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

} // namespace

namespace ZipArchive {

bool write(const QString &path, const QMap<QString, QByteArray> &entries, QString *error)
{
    if (entries.isEmpty() || entries.size() > std::numeric_limits<quint16>::max())
        return fail(error, QStringLiteral("Invalid number of project entries."));

    QByteArray archive;
    QByteArray directory;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        const QByteArray name = it.key().toUtf8();
        const QByteArray &content = it.value();
        if (name.isEmpty() || name.size() > std::numeric_limits<quint16>::max()
            || content.size() > maximumEntrySize || archive.size() > std::numeric_limits<quint32>::max())
            return fail(error, QStringLiteral("A project entry is too large."));

        QByteArray compressed;
        if (!compress(content, &compressed))
            return fail(error, QStringLiteral("Could not compress a project entry."));

        const quint32 offset = static_cast<quint32>(archive.size());
        const quint32 crc = checksum(content);
        const quint32 compressedSize = static_cast<quint32>(compressed.size());
        const quint32 originalSize = static_cast<quint32>(content.size());

        append32(archive, localSignature);
        append16(archive, 20);
        append16(archive, utf8Flag);
        append16(archive, deflateMethod);
        append16(archive, 0);
        append16(archive, 0);
        append32(archive, crc);
        append32(archive, compressedSize);
        append32(archive, originalSize);
        append16(archive, static_cast<quint16>(name.size()));
        append16(archive, 0);
        archive.append(name);
        archive.append(compressed);

        append32(directory, centralSignature);
        append16(directory, 20);
        append16(directory, 20);
        append16(directory, utf8Flag);
        append16(directory, deflateMethod);
        append16(directory, 0);
        append16(directory, 0);
        append32(directory, crc);
        append32(directory, compressedSize);
        append32(directory, originalSize);
        append16(directory, static_cast<quint16>(name.size()));
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append16(directory, 0);
        append32(directory, 0);
        append32(directory, offset);
        directory.append(name);
    }

    if (archive.size() + directory.size() + 22 > std::numeric_limits<quint32>::max())
        return fail(error, QStringLiteral("Project archive exceeds ZIP32 limits."));

    const quint32 directoryOffset = static_cast<quint32>(archive.size());
    archive.append(directory);
    append32(archive, endSignature);
    append16(archive, 0);
    append16(archive, 0);
    append16(archive, static_cast<quint16>(entries.size()));
    append16(archive, static_cast<quint16>(entries.size()));
    append32(archive, static_cast<quint32>(directory.size()));
    append32(archive, directoryOffset);
    append16(archive, 0);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, file.errorString());
    if (file.write(archive) != archive.size() || !file.commit())
        return fail(error, file.errorString());
    return true;
}

bool read(const QString &path, QMap<QString, QByteArray> *entries, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, file.errorString());
    if (file.size() > maximumArchiveSize)
        return fail(error, QStringLiteral("Project archive is too large."));
    const QByteArray archive = file.readAll();
    if (archive.size() < 22 || archive.size() != file.size())
        return fail(error, QStringLiteral("Incomplete project archive."));

    qint64 endOffset = -1;
    for (qint64 offset = archive.size() - 22;
         offset >= qMax<qint64>(0, archive.size() - 22 - 65535); --offset) {
        quint32 signature = 0;
        quint16 commentSize = 0;
        if (read32(archive, offset, &signature) && signature == endSignature
            && read16(archive, offset + 20, &commentSize)
            && offset + 22 + commentSize == archive.size()) {
            endOffset = offset;
            break;
        }
    }
    if (endOffset < 0)
        return fail(error, QStringLiteral("Missing ZIP directory."));

    quint16 disk = 0, directoryDisk = 0, diskEntries = 0, count = 0;
    quint32 directorySize = 0, directoryOffset = 0;
    read16(archive, endOffset + 4, &disk);
    read16(archive, endOffset + 6, &directoryDisk);
    read16(archive, endOffset + 8, &diskEntries);
    read16(archive, endOffset + 10, &count);
    read32(archive, endOffset + 12, &directorySize);
    read32(archive, endOffset + 16, &directoryOffset);
    if (disk != 0 || directoryDisk != 0 || diskEntries != count || count == 0 || count > 16
        || static_cast<quint64>(directoryOffset) + directorySize > static_cast<quint64>(endOffset))
        return fail(error, QStringLiteral("Unsupported ZIP directory."));

    QMap<QString, QByteArray> decoded;
    quint64 totalUncompressed = 0;
    qint64 cursor = directoryOffset;
    for (int index = 0; index < count; ++index) {
        quint32 signature = 0;
        quint16 flags = 0, method = 0, nameSize = 0, extraSize = 0, commentSize = 0;
        quint32 crc = 0, compressedSize = 0, originalSize = 0, localOffset = 0;
        if (!read32(archive, cursor, &signature) || signature != centralSignature
            || !read16(archive, cursor + 8, &flags) || !read16(archive, cursor + 10, &method)
            || !read32(archive, cursor + 16, &crc)
            || !read32(archive, cursor + 20, &compressedSize)
            || !read32(archive, cursor + 24, &originalSize)
            || !read16(archive, cursor + 28, &nameSize)
            || !read16(archive, cursor + 30, &extraSize)
            || !read16(archive, cursor + 32, &commentSize)
            || !read32(archive, cursor + 42, &localOffset)
            || cursor + 46ll + nameSize + extraSize + commentSize > archive.size()
            || originalSize > maximumEntrySize || (flags & ~utf8Flag) != 0
            || (method != deflateMethod && method != storedMethod))
            return fail(error, QStringLiteral("Unsupported or damaged ZIP entry."));

        totalUncompressed += originalSize;
        if (totalUncompressed > static_cast<quint64>(maximumArchiveSize))
            return fail(error, QStringLiteral("Project entries are too large."));

        const QByteArray nameBytes = archive.mid(cursor + 46, nameSize);
        const QString name = QString::fromUtf8(nameBytes);
        if (name.toUtf8() != nameBytes || name.isEmpty() || decoded.contains(name))
            return fail(error, QStringLiteral("Invalid or duplicate ZIP entry name."));

        quint32 localHeader = 0;
        quint16 localFlags = 0, localMethod = 0, localNameSize = 0, localExtraSize = 0;
        if (!read32(archive, localOffset, &localHeader) || localHeader != localSignature
            || !read16(archive, localOffset + 6, &localFlags)
            || !read16(archive, localOffset + 8, &localMethod)
            || !read16(archive, localOffset + 26, &localNameSize)
            || !read16(archive, localOffset + 28, &localExtraSize)
            || localFlags != flags || localMethod != method || localNameSize != nameSize)
            return fail(error, QStringLiteral("Invalid local ZIP entry."));

        const qint64 dataOffset = static_cast<qint64>(localOffset) + 30 + localNameSize + localExtraSize;
        if (dataOffset + compressedSize > directoryOffset
            || archive.mid(localOffset + 30, localNameSize) != nameBytes)
            return fail(error, QStringLiteral("ZIP entry extends beyond its data area."));

        QByteArray content;
        const QByteArray payload = archive.mid(dataOffset, compressedSize);
        if (method == storedMethod) {
            if (compressedSize != originalSize)
                return fail(error, QStringLiteral("Stored ZIP entry has an invalid size."));
            content = payload;
        } else if (!decompress(payload, originalSize, &content)) {
            return fail(error, QStringLiteral("ZIP entry could not be decompressed."));
        }
        if (checksum(content) != crc)
            return fail(error, QStringLiteral("ZIP entry failed its checksum."));
        decoded.insert(name, content);
        cursor += 46ll + nameSize + extraSize + commentSize;
    }
    if (cursor != static_cast<qint64>(directoryOffset) + directorySize)
        return fail(error, QStringLiteral("ZIP directory length does not match."));

    *entries = decoded;
    return true;
}

} // namespace ZipArchive
