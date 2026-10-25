#pragma once

#include "../src/codeplugreader.h"
#include "../src/codeplugwriter.h"
#include "../src/cs7000default.h"

inline QByteArray channelOrderFixture(QString *error)
{
    const QByteArray base = cs7000DefaultCodeplug();
    ImportedCodeplug target;
    if (!CodeplugReader::read(base, &target, error))
        return {};
    target.contacts = {{QStringLiteral("Alpha"), 9, true},
                       {QStringLiteral("Bravo"), 3100, true}};
    target.channels = {
        {QStringLiteral("TS1 Alpha 1"), 449350000, 444350000, true, 2, 1, 1, true},
        {QStringLiteral("TS1 Bravo"),   449350000, 444350000, true, 2, 1, 2, true},
        {QStringLiteral("TS1 Alpha 2"), 449350000, 444350000, true, 2, 1, 1, true},
        {QStringLiteral("TS2 Alpha"),   449350000, 444350000, true, 2, 2, 1, true},
        {QStringLiteral("TS2 Bravo"),   449350000, 444350000, true, 2, 2, 2, true},
        {QStringLiteral("Simplex"),     446000000, 446000000, true, 1, 2, 1, false}
    };
    target.zones = {
        {QStringLiteral("Ordering test"), {3, 0, 1, 4, 2, 5}},
        {QStringLiteral("Shared channels"), {2, 3}},
        {QStringLiteral("Simplex zone"), {5}}
    };
    QByteArray source;
    if (!CodeplugWriter::write(base, target, QStringLiteral("TEST RADIO"), 3109101, &source, error))
        return {};
    // Unknown settings and channel bytes must survive an ordering-only export.
    source[0x600] = char(uchar(source[0x600]) ^ 0x5a);
    source[0x23768 + 48 * 2 + 13] = char(uchar(source[0x23768 + 48 * 2 + 13]) ^ 0xa5);
    return source;
}
