#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <optional>

struct ImportedChannel {
    QString name;
    quint32 frequencyAHz = 0;
    quint32 frequencyBHz = 0;
    bool digital = false;
    int colorCode = 0;
    int timeSlot = 0;
    int contactIndex = 0; // One-based contact table index; zero means no contact.
    std::optional<bool> highPower; // CS7000 channel power; absent in older saved projects.
    int sourceRecord = -1; // Original channel record in the retained export base.
    bool useDefaultTemplate = false; // Mode changes start from the matching mode defaults.
    QString powerSetting; // Definition enum value for settings beyond legacy High/Low.
    QString powerValue() const { return highPower ? (*highPower ? QStringLiteral("high") : QStringLiteral("low")) : powerSetting; }
    void setPowerValue(const QString &value) {
        powerSetting = value;
        highPower = value == "high" ? std::optional<bool>(true)
            : value == "low" ? std::optional<bool>(false) : std::nullopt;
    }
};

struct ImportedContact {
    QString name;
    int id = 0;
    bool groupCall = false;
    int sourceRecord = -1;
    QString callType;
    QString callTypeValue() const { return callType.isEmpty() || callType == "group" || callType == "private" ? (groupCall ? QStringLiteral("group") : QStringLiteral("private")) : callType; }
};

struct ImportedZone {
    QString name;
    QVector<int> channelIndexes;
    std::optional<ImportedChannel> repeaterTemplate; // Settings for a newly created, empty repeater.
    int sourceRecord = -1;
};

struct ImportedCodeplug {
    QString definitionFile; // Selected disk definition basename, retained with imported projects.
    QString model;
    QString cpsVersion;
    QString radioName;
    int radioId = 0;
    QVector<ImportedChannel> channels;
    QVector<ImportedContact> contacts;
    QVector<ImportedZone> zones;
    QStringList powerSettings;
    int channelCapacity = 3000;
    int contactCapacity = 10000;
    int zoneCapacity = 250;
    int zoneMemberCapacity = 16;
};

class CodeplugReader
{
public:
    // Reads the observed CS7000-M17 Plus CPS V1.02.20.00 and V1.02.21.00 layouts.
    static bool read(const QByteArray &bytes, ImportedCodeplug *result, QString *error);
};
