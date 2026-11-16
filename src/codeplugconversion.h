#pragma once
#include "importedcodeplug.h"
#include <QJsonObject>
#include <QSet>
#include <QVariantMap>

namespace CodeplugConversion {
struct Session {
    ImportedCodeplug source;
    ImportedCodeplug baseline;
    ImportedCodeplug converted;
    QJsonObject metadata;
    QVector<int> contactUses;
    QByteArray targetBase;
    QByteArray prepared;
    QSet<int> contacts, channels, zones;
    bool twoMeters = false;
    bool seventyCm = false;
    quint64 sourceRevision = 0;
    QVariantMap review;
};
void rebuild(Session &session, bool initialize = false);
}
