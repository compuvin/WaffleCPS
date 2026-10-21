#pragma once

#include <QByteArray>
#include <QString>
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
};

struct ImportedContact {
    QString name;
    int id = 0;
    bool groupCall = false;
};

struct ImportedZone {
    QString name;
    QVector<int> channelIndexes;
};

struct ImportedCodeplug {
    QString model;
    QString cpsVersion;
    QString radioName;
    int radioId = 0;
    QVector<ImportedChannel> channels;
    QVector<ImportedContact> contacts;
    QVector<ImportedZone> zones;
};

class CodeplugReader
{
public:
    // Reads the observed CS7000-M17 Plus CPS V1.02.20.00 and V1.02.21.00 layouts.
    static bool read(const QByteArray &bytes, ImportedCodeplug *result, QString *error);
};
