#include "repeaterstore.h"
#include "codeplugwriter.h"
#include "cs7000default.h"
#include "ziparchive.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

namespace {

const QString projectFormat = QStringLiteral("org.wafflecps.project");
const QString sourceEntry = QStringLiteral("source/original-codeplug.bin");
constexpr qint64 maximumSourceSize = 256ll * 1024ll * 1024ll;

bool integerValue(const QJsonValue &value, qint64 minimum, qint64 maximum, qint64 *result)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum || std::floor(number) != number)
        return false;
    *result = static_cast<qint64>(number);
    return true;
}

void makeExportChannelNamesUnique(ImportedCodeplug *codeplug)
{
    QSet<QString> reserved;
    QSet<QString> used;
    for (const auto &channel : codeplug->channels)
        reserved.insert(channel.name.toCaseFolded());
    for (auto &channel : codeplug->channels) {
        const QString key = channel.name.toCaseFolded();
        if (!used.contains(key)) {
            used.insert(key);
            continue;
        }
        for (int number = 2; ; ++number) {
            const QString suffix = QStringLiteral(" %1").arg(number);
            const QString candidate = channel.name.left(16 - suffix.size()) + suffix;
            const QString candidateKey = candidate.toCaseFolded();
            if (!reserved.contains(candidateKey) && !used.contains(candidateKey)) {
                channel.name = candidate;
                used.insert(candidateKey);
                break;
            }
        }
    }
}

} // namespace

RepeaterStore::RepeaterStore(QObject *parent)
    : QObject(parent)
    , m_repeaters{
          {QStringLiteral("North Ridge"), 444500000, 449500000, 1,
           {{QStringLiteral("Local"), 9, QStringLiteral("North Local")},
            {QStringLiteral("Statewide"), 123, QStringLiteral("North Statewide")}},
           {{QStringLiteral("Regional"), 3100, QStringLiteral("North Regional")}}},
          {QStringLiteral("Riverbend"), 442125000, 447125000, 3,
           {{QStringLiteral("Local"), 9, QStringLiteral("Riverbend Local")}},
           {{QStringLiteral("Worldwide"), 91, QStringLiteral("Riverbend Worldw")},
            {QStringLiteral("Regional"), 3100, QStringLiteral("Riverbend Regio")}}}}
{
}

QStringList RepeaterStore::repeaterNames() const
{
    QStringList names;
    for (const auto &repeater : m_repeaters)
        names.append(repeater.name);
    return names;
}

int RepeaterStore::selectedIndex() const { return m_selectedIndex; }
QString RepeaterStore::selectedName() const { return m_repeaters.empty() ? QString() : selected().name; }
QString RepeaterStore::rxFrequency() const { return m_repeaters.empty() ? QString() : formatFrequency(selected().rxHz); }
QString RepeaterStore::txFrequency() const { return m_repeaters.empty() ? QString() : formatFrequency(selected().txHz); }
int RepeaterStore::colorCode() const { return m_repeaters.empty() ? 0 : selected().colorCode; }
QString RepeaterStore::selectedPower() const
{
    return m_repeaters.empty() ? QString() : (selected().highPower ? QStringLiteral("High") : QStringLiteral("Low"));
}
QString RepeaterStore::radioName() const { return m_radioName; }
int RepeaterStore::radioId() const { return m_radioId; }
QString RepeaterStore::radioModel() const { return m_importedCodeplug.model; }
QString RepeaterStore::radioCpsVersion() const { return m_importedCodeplug.cpsVersion; }

QString RepeaterStore::availableChannelName(const QString &channelName) const
{
    const QString base = channelName.trimmed().left(16);
    if (base.isEmpty())
        return {};
    QSet<QString> names;
    if (hasImportedCodeplug()) {
        for (const auto &channel : m_importedCodeplug.channels)
            names.insert(channel.name.toCaseFolded());
    } else {
        for (const auto &repeater : m_repeaters) {
            for (const auto &group : repeater.slot1)
                names.insert((group.channelName.isEmpty() ? group.name : group.channelName).toCaseFolded());
            for (const auto &group : repeater.slot2)
                names.insert((group.channelName.isEmpty() ? group.name : group.channelName).toCaseFolded());
            for (const auto &simplex : repeater.simplexFrequencies)
                names.insert(simplex.name.toCaseFolded());
        }
    }
    if (!names.contains(base.toCaseFolded()))
        return base;
    for (int number = 2; ; ++number) {
        const QString suffix = QStringLiteral(" %1").arg(number);
        const QString candidate = base.left(16 - suffix.size()) + suffix;
        if (!names.contains(candidate.toCaseFolded()))
            return candidate;
    }
}

QString RepeaterStore::suggestedChannelName(const QString &talkgroupName) const
{
    const QString repeaterName = hasImportedCodeplug()
        ? selectedImportedGroup().value(QStringLiteral("name")).toString() : selectedName();
    QString firstWord = repeaterName.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                    Qt::SkipEmptyParts).value(0);
    if (firstWord.size() > 14)
        firstWord = firstWord.left(8);
    const QString name = firstWord.isEmpty() ? talkgroupName.trimmed()
                                             : firstWord + QLatin1Char(' ') + talkgroupName.trimmed();
    return availableChannelName(name);
}

bool RepeaterStore::updateRadioSettings(const QString &name, const QString &idText)
{
    const QString cleanName = name.trimmed();
    const QString cleanId = idText.trimmed();
    bool validId = false;
    const int id = cleanId.toInt(&validId);
    if (cleanName.isEmpty() || cleanName.size() > 16)
        return fail(QStringLiteral("Radio name or call sign must be 1–16 characters."));
    if (!validId || id < 1 || id > 16777215 || cleanId != QString::number(id))
        return fail(QStringLiteral("Radio ID must be a number from 1 to 16777215."));
    if (m_radioName == cleanName && m_radioId == id)
        return true;
    m_radioName = cleanName;
    m_radioId = id;
    emit radioSettingsChanged();
    setModified(true);
    return true;
}
QString RepeaterStore::projectDisplayName() const
{
    return m_projectPath.isEmpty() ? QStringLiteral("Untitled project") : QFileInfo(m_projectPath).fileName();
}
QString RepeaterStore::sourceCodeplugName() const { return m_sourceCodeplugName; }
bool RepeaterStore::modified() const { return m_modified; }
bool RepeaterStore::hasImportedCodeplug() const { return !m_importedCodeplug.channels.isEmpty(); }
QString RepeaterStore::importedCodeplugSummary() const
{
    if (!hasImportedCodeplug())
        return {};
    return QStringLiteral("%1 · %2 · %3 channels · %4 zones")
        .arg(m_importedCodeplug.model, m_importedCodeplug.cpsVersion)
        .arg(m_importedCodeplug.channels.size())
        .arg(m_importedCodeplug.zones.size());
}

QStringList RepeaterStore::importedRepeaterNames() const
{
    QStringList names;
    for (const auto &group : m_importedGroups)
        if (group.repeater)
            names.append(group.name);
    return names;
}

QStringList RepeaterStore::importedZoneNames() const
{
    QStringList names;
    for (const auto &group : m_importedGroups)
        if (!group.repeater)
            names.append(group.name);
    return names;
}

int RepeaterStore::selectedImportedIndex() const { return m_selectedImportedIndex; }

QVariantMap RepeaterStore::selectedImportedGroup() const
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return {};
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    QVariantList zones;
    QVariantList additionalSimplex;
    QVariantList slot1Channels;
    QVariantList slot2Channels;
    QVariantList slot1Talkgroups;
    QVariantList slot2Talkgroups;
    int zoneChannelCount = 0;
    std::optional<bool> repeaterPower;
    bool mixedPower = false;
    bool unknownPower = false;
    int highPowerChannels = 0;
    int lowPowerChannels = 0;
    for (int zoneInGroup = 0; zoneInGroup < group.zoneIndexes.size(); ++zoneInGroup) {
        const int zoneIndex = group.zoneIndexes.at(zoneInGroup);
        const auto &zone = m_importedCodeplug.zones.at(zoneIndex);
        zoneChannelCount += zone.channelIndexes.size();
        QVariantList channels;
        QVariantList repeaterChannels;
        for (int zonePosition = 0; zonePosition < zone.channelIndexes.size(); ++zonePosition) {
            const int channelIndex = zone.channelIndexes.at(zonePosition);
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            QVariantMap item{
                {QStringLiteral("name"), channel.name},
                {QStringLiteral("number"), channelIndex + 1},
                {QStringLiteral("zonePosition"), zonePosition + 1},
                {QStringLiteral("contactIndex"), channel.contactIndex},
                {QStringLiteral("receiveFrequency"), formatFrequency(channel.frequencyBHz)},
                {QStringLiteral("transmitFrequency"), formatFrequency(channel.frequencyAHz)},
                {QStringLiteral("mode"), channel.digital ? QStringLiteral("Digital") : QStringLiteral("Analog / other")},
                {QStringLiteral("colorCode"), channel.digital ? QVariant(channel.colorCode) : QVariant()},
                {QStringLiteral("timeSlot"), channel.digital ? QVariant(channel.timeSlot) : QVariant()},
                {QStringLiteral("power"), channel.highPower.has_value()
                     ? (*channel.highPower ? QStringLiteral("High") : QStringLiteral("Low"))
                     : QStringLiteral("Unknown")}};
            if (channel.digital && channel.contactIndex > 0
                && channel.contactIndex <= m_importedCodeplug.contacts.size()) {
                const auto &contact = m_importedCodeplug.contacts.at(channel.contactIndex - 1);
                item.insert(QStringLiteral("contactName"), contact.name);
                item.insert(QStringLiteral("contactId"), contact.id);
                item.insert(QStringLiteral("contactGroupCall"), contact.groupCall);
            }
            channels.append(item);
            if (group.repeater && channel.frequencyAHz == channel.frequencyBHz) {
                item.insert(QStringLiteral("zoneName"), zone.name);
                item.insert(QStringLiteral("zoneInGroup"), zoneInGroup);
                additionalSimplex.append(item);
            } else {
                repeaterChannels.append(item);
                if (group.repeater) {
                    if (!channel.highPower.has_value())
                        unknownPower = true;
                    else {
                        if (*channel.highPower)
                            ++highPowerChannels;
                        else
                            ++lowPowerChannels;
                        if (repeaterPower.has_value() && repeaterPower != channel.highPower)
                            mixedPower = true;
                        else
                            repeaterPower = channel.highPower;
                    }
                }
                if (group.repeater && channel.timeSlot == 1)
                    slot1Channels.append(item);
                else if (group.repeater && channel.timeSlot == 2)
                    slot2Channels.append(item);
            }
        }
        zones.append(QVariantMap{{QStringLiteral("name"), zone.name},
                                 {QStringLiteral("channels"), channels},
                                 {QStringLiteral("repeaterChannels"), repeaterChannels}});
    }
    auto makeTalkgroups = [this](const QVariantList &channelItems) {
        QVariantList groups;
        QMap<int, int> indexes;
        for (const QVariant &value : channelItems) {
            const QVariantMap channel = value.toMap();
            const int contactIndex = m_importedCodeplug.channels.at(channel.value(QStringLiteral("number")).toInt() - 1).contactIndex;
            const bool valid = contactIndex > 0 && contactIndex <= m_importedCodeplug.contacts.size();
            const int key = valid ? contactIndex : 0;
            int groupIndex = indexes.value(key, -1);
            if (groupIndex < 0) {
                groupIndex = groups.size();
                indexes.insert(key, groupIndex);
                groups.append(QVariantMap{{QStringLiteral("name"), valid ? m_importedCodeplug.contacts.at(contactIndex - 1).name
                                                                : QStringLiteral("No contact assigned")},
                                          {QStringLiteral("contactIndex"), key},
                                          {QStringLiteral("id"), valid ? m_importedCodeplug.contacts.at(contactIndex - 1).id : 0},
                                          {QStringLiteral("groupCall"), valid && m_importedCodeplug.contacts.at(contactIndex - 1).groupCall},
                                          {QStringLiteral("channels"), QVariantList{}}});
            }
            QVariantMap groupItem = groups.at(groupIndex).toMap();
            QVariantList children = groupItem.value(QStringLiteral("channels")).toList();
            children.append(channel);
            groupItem.insert(QStringLiteral("channels"), children);
            groups[groupIndex] = groupItem;
        }
        return groups;
    };
    slot1Talkgroups = makeTalkgroups(slot1Channels);
    slot2Talkgroups = makeTalkgroups(slot2Channels);
    return {{QStringLiteral("name"), group.name},
            {QStringLiteral("classification"), group.reason},
            {QStringLiteral("isRepeater"), group.repeater},
            {QStringLiteral("receiveFrequency"), group.repeater ? formatFrequency(group.frequencyBHz) : QString()},
            {QStringLiteral("transmitFrequency"), group.repeater ? formatFrequency(group.frequencyAHz) : QString()},
            {QStringLiteral("colorCode"), group.colorCode},
            {QStringLiteral("power"), mixedPower ? QStringLiteral("Mixed")
                 : unknownPower ? QStringLiteral("Unknown")
                 : repeaterPower.value_or(true) ? QStringLiteral("High") : QStringLiteral("Low")},
            {QStringLiteral("newChannelPower"), highPowerChannels >= lowPowerChannels
                 ? QStringLiteral("High") : QStringLiteral("Low")},
            {QStringLiteral("simplexChannels"), additionalSimplex},
            {QStringLiteral("slot1Channels"), slot1Channels},
            {QStringLiteral("slot2Channels"), slot2Channels},
            {QStringLiteral("slot1Talkgroups"), slot1Talkgroups},
            {QStringLiteral("slot2Talkgroups"), slot2Talkgroups},
            {QStringLiteral("zoneChannelCount"), zoneChannelCount},
            {QStringLiteral("zones"), zones}};
}

void RepeaterStore::selectImportedGroup(int index)
{
    if (index < 0 || index >= m_importedGroups.size() || index == m_selectedImportedIndex)
        return;
    m_selectedImportedIndex = index;
    emit importedSelectionChanged();
}

bool RepeaterStore::updateSelectedImportedRepeater(const QString &name, const QString &rxFrequency,
                                                   const QString &txFrequency, int colorCode,
                                                   int powerChoice)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0, txHz = 0;
    if (!group.repeater || group.zoneIndexes.size() != 1 || cleanName.isEmpty() || cleanName.size() > 16
        || !parseFrequency(rxFrequency, &rxHz) || !parseFrequency(txFrequency, &txHz)
        || rxHz < 100000000 || txHz < 100000000 || rxHz == txHz
        || colorCode < 0 || colorCode > 15 || powerChoice < -1 || powerChoice > 1)
        return false;
    const int zoneIndex = group.zoneIndexes.constFirst();
    auto &zone = m_importedCodeplug.zones[zoneIndex];
    auto needsChange = [txHz, rxHz, colorCode, powerChoice](const ImportedChannel &channel) {
        return channel.frequencyAHz != channel.frequencyBHz
            && (channel.frequencyAHz != txHz || channel.frequencyBHz != rxHz
                || channel.colorCode != colorCode
                || (powerChoice >= 0 && channel.highPower != std::optional<bool>(powerChoice == 1)));
    };
    const bool changeChannels = std::any_of(zone.channelIndexes.cbegin(), zone.channelIndexes.cend(),
        [this, &needsChange](int index) { return needsChange(m_importedCodeplug.channels.at(index)); });
    if (!changeChannels && zone.name == cleanName)
        return true;
    if (changeChannels) {
        QSet<int> sharedChannels;
        for (int channelIndex : zone.channelIndexes) {
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            if (!needsChange(channel) || sharedChannels.contains(channelIndex))
                continue;
            for (int otherZone = 0; otherZone < m_importedCodeplug.zones.size(); ++otherZone)
                if (otherZone != zoneIndex
                    && m_importedCodeplug.zones.at(otherZone).channelIndexes.contains(channelIndex)) {
                    sharedChannels.insert(channelIndex);
                    break;
                }
        }
        if (m_importedCodeplug.channels.size() + sharedChannels.size() > 3000)
            return false;
    }
    QMap<int, int> clones;
    for (int &channelIndex : zone.channelIndexes) {
        const ImportedChannel channel = m_importedCodeplug.channels.at(channelIndex);
        if (!needsChange(channel))
            continue;
        if (clones.contains(channelIndex)) {
            channelIndex = clones.value(channelIndex);
            continue;
        }
        const int originalIndex = channelIndex;
        bool shared = false;
        for (int otherZone = 0; otherZone < m_importedCodeplug.zones.size() && !shared; ++otherZone)
            if (otherZone != zoneIndex)
                shared = m_importedCodeplug.zones.at(otherZone).channelIndexes.contains(originalIndex);
        if (shared) {
            if (m_importedCodeplug.channels.size() >= 3000)
                return false;
            m_importedCodeplug.channels.append(channel);
            channelIndex = m_importedCodeplug.channels.size() - 1;
        }
        clones.insert(originalIndex, channelIndex);
        auto &editable = m_importedCodeplug.channels[channelIndex];
        editable.frequencyAHz = static_cast<quint32>(txHz);
        editable.frequencyBHz = static_cast<quint32>(rxHz);
        editable.colorCode = colorCode;
        if (powerChoice >= 0)
            editable.highPower = powerChoice == 1;
    }
    zone.name = cleanName;
    rebuildImportedGroups();
    for (int index = 0; index < m_importedGroups.size(); ++index)
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addImportedTalkgroup(int timeSlot, const QString &name, int id,
                                         const QString &channelName)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || group.zoneIndexes.size() != 1 || (timeSlot != 1 && timeSlot != 2)
        || id < 1 || id > 16777215 || m_importedCodeplug.channels.size() >= 3000)
        return false;
    const int zoneIndex = group.zoneIndexes.constFirst();
    auto &zone = m_importedCodeplug.zones[zoneIndex];
    const QString cleanName = name.trimmed();
    const QString cleanChannelName = channelName.trimmed().isEmpty() ? cleanName : channelName.trimmed();
    if (zone.channelIndexes.size() >= 16 || cleanName.isEmpty() || cleanName.size() > 16
        || cleanChannelName.isEmpty() || cleanChannelName.size() > 16
        || availableChannelName(cleanChannelName) != cleanChannelName)
        return false;
    const QVariantMap powerSummary = selectedImportedGroup();
    if (powerSummary.value(QStringLiteral("power")).toString() == QStringLiteral("Unknown"))
        return false;
    const bool highPower = powerSummary.value(QStringLiteral("newChannelPower")).toString() == QStringLiteral("High");
    int contactIndex = 0;
    for (int index = 0; index < m_importedCodeplug.contacts.size(); ++index) {
        const auto &contact = m_importedCodeplug.contacts.at(index);
        if (contact.id == id) {
            if (!contact.groupCall || contact.name.simplified() != cleanName.simplified())
                return false;
            contactIndex = index + 1;
            break;
        }
    }
    for (int channelIndex : zone.channelIndexes) {
        const auto &channel = m_importedCodeplug.channels.at(channelIndex);
        if (channel.digital && channel.frequencyAHz == group.frequencyAHz
            && channel.frequencyBHz == group.frequencyBHz && channel.colorCode == group.colorCode
            && channel.timeSlot == timeSlot && channel.contactIndex == contactIndex
            && contactIndex != 0)
            return false;
    }
    if (contactIndex == 0) {
        if (m_importedCodeplug.contacts.size() >= 10000)
            return false;
        m_importedCodeplug.contacts.append({cleanName, id, true});
        contactIndex = m_importedCodeplug.contacts.size();
    }
    m_importedCodeplug.channels.append({cleanChannelName, group.frequencyAHz, group.frequencyBHz,
                                       true, group.colorCode, timeSlot, contactIndex, highPower});
    zone.channelIndexes.append(m_importedCodeplug.channels.size() - 1);
    rebuildImportedGroups();
    emit contactsChanged();
    for (int index = 0; index < m_importedGroups.size(); ++index)
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::removeImportedTalkgroup(int timeSlot, int contactIndex)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || group.zoneIndexes.size() != 1 || (timeSlot != 1 && timeSlot != 2)
        || contactIndex < 1 || contactIndex > m_importedCodeplug.contacts.size())
        return false;
    const int zoneIndex = group.zoneIndexes.constFirst();
    auto &members = m_importedCodeplug.zones[zoneIndex].channelIndexes;
    bool removed = false;
    QVector<int> removedChannelIndexes;
    for (int position = members.size() - 1; position >= 0; --position) {
        const int channelIndex = members.at(position);
        const auto &channel = m_importedCodeplug.channels.at(channelIndex);
        if (channel.digital && channel.frequencyAHz == group.frequencyAHz
            && channel.frequencyBHz == group.frequencyBHz && channel.colorCode == group.colorCode
            && channel.timeSlot == timeSlot && channel.contactIndex == contactIndex) {
            members.removeAt(position);
            removedChannelIndexes.append(channelIndex);
            removed = true;
        }
    }
    if (!removed)
        return false;
    std::sort(removedChannelIndexes.begin(), removedChannelIndexes.end(), std::greater<int>());
    removedChannelIndexes.erase(std::unique(removedChannelIndexes.begin(), removedChannelIndexes.end()),
                                removedChannelIndexes.end());
    for (int channelIndex : removedChannelIndexes) {
        bool usedElsewhere = false;
        for (const auto &zone : m_importedCodeplug.zones) {
            if (zone.channelIndexes.contains(channelIndex)) {
                usedElsewhere = true;
                break;
            }
        }
        if (usedElsewhere)
            continue;
        m_importedCodeplug.channels.removeAt(channelIndex);
        for (auto &zone : m_importedCodeplug.zones)
            for (int &memberIndex : zone.channelIndexes)
                if (memberIndex > channelIndex)
                    --memberIndex;
    }
    rebuildImportedGroups();
    for (int index = 0; index < m_importedGroups.size(); ++index)
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addSimplexToSelectedImportedRepeater(int zoneInGroup, const QString &name,
                                                        const QString &frequency, bool digital,
                                                        int colorCode, int timeSlot)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || zoneInGroup < 0 || zoneInGroup >= group.zoneIndexes.size()
        || m_importedCodeplug.channels.size() >= 3000)
        return false;
    const int zoneIndex = group.zoneIndexes.at(zoneInGroup);
    if (m_importedCodeplug.zones.at(zoneIndex).channelIndexes.size() >= 16)
        return false;
    qint64 hz = 0;
    const QString cleanName = name.trimmed();
    if (cleanName.isEmpty() || cleanName.size() > 16 || !parseFrequency(frequency, &hz)
        || hz < 100000000
        || (digital && (colorCode < 0 || colorCode > 15 || timeSlot < 1 || timeSlot > 2)))
        return false;
    const int channelIndex = m_importedCodeplug.channels.size();
    m_importedCodeplug.channels.append({cleanName, static_cast<quint32>(hz),
                                       static_cast<quint32>(hz), digital,
                                       digital ? colorCode : 0, digital ? timeSlot : 0});
    m_importedCodeplug.zones[zoneIndex].channelIndexes.append(channelIndex);
    const int previousIndex = m_selectedImportedIndex;
    rebuildImportedGroups();
    m_selectedImportedIndex = previousIndex;
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::removeSimplexFromSelectedImportedRepeater(int zoneInGroup, int zonePosition)
{
    if (m_selectedImportedIndex < 0 || m_selectedImportedIndex >= m_importedGroups.size())
        return false;
    const auto &group = m_importedGroups.at(m_selectedImportedIndex);
    if (!group.repeater || zoneInGroup < 0 || zoneInGroup >= group.zoneIndexes.size())
        return false;
    const int zoneIndex = group.zoneIndexes.at(zoneInGroup);
    auto &members = m_importedCodeplug.zones[zoneIndex].channelIndexes;
    const int position = zonePosition - 1;
    if (position < 0 || position >= members.size())
        return false;
    const int channelIndex = members.at(position);
    const auto &channel = m_importedCodeplug.channels.at(channelIndex);
    if (channel.frequencyAHz != channel.frequencyBHz)
        return false;

    members.removeAt(position);
    bool usedElsewhere = false;
    for (const auto &zone : m_importedCodeplug.zones) {
        if (zone.channelIndexes.contains(channelIndex)) {
            usedElsewhere = true;
            break;
        }
    }
    if (!usedElsewhere) {
        m_importedCodeplug.channels.removeAt(channelIndex);
        for (auto &zone : m_importedCodeplug.zones)
            for (int &memberIndex : zone.channelIndexes)
                if (memberIndex > channelIndex)
                    --memberIndex;
    }
    rebuildImportedGroups();
    for (int index = 0; index < m_importedGroups.size(); ++index) {
        if (m_importedGroups.at(index).zoneIndexes.contains(zoneIndex)) {
            m_selectedImportedIndex = index;
            break;
        }
    }
    emit importedSelectionChanged();
    setModified(true);
    return true;
}

void RepeaterStore::rebuildImportedGroups()
{
    m_importedGroups.clear();
    QVector<ImportedGroup> remainingZones;
    for (int zoneIndex = 0; zoneIndex < m_importedCodeplug.zones.size(); ++zoneIndex) {
        const auto &zone = m_importedCodeplug.zones.at(zoneIndex);
        bool repeater = true;
        bool hasRepeaterChannel = false;
        QString reason;
        ImportedChannel first;
        for (int channelIndex : zone.channelIndexes) {
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            if (channel.frequencyAHz == channel.frequencyBHz) {
                continue;
            }
            if (!channel.digital) {
                repeater = false;
                reason = QStringLiteral("Contains an analog or unrecognized non-simplex channel");
            } else if (!hasRepeaterChannel) {
                first = channel;
                hasRepeaterChannel = true;
            } else if (channel.frequencyAHz != first.frequencyAHz
                       || channel.frequencyBHz != first.frequencyBHz) {
                repeater = false;
                reason = QStringLiteral("Channels use different repeater frequency pairs");
            } else if (channel.colorCode != first.colorCode) {
                repeater = false;
                reason = QStringLiteral("Repeater channels use different color codes");
            }
        }
        repeater = repeater && hasRepeaterChannel;
        if (!hasRepeaterChannel)
            reason = zone.channelIndexes.isEmpty()
                ? QStringLiteral("Empty zone")
                : QStringLiteral("Only same-frequency channels; may be simplex or a hotspot");
        if (repeater) {
            ImportedGroup group;
            group.name = zone.name;
            group.reason = QStringLiteral("One repeater frequency pair and color code; simplex channels shown separately");
            group.zoneIndexes.append(zoneIndex);
            group.repeater = true;
            group.frequencyAHz = first.frequencyAHz;
            group.frequencyBHz = first.frequencyBHz;
            group.colorCode = first.colorCode;
            m_importedGroups.append(group);
        } else {
            ImportedGroup group;
            group.name = zone.name;
            group.reason = reason;
            group.zoneIndexes.append(zoneIndex);
            remainingZones.append(group);
        }
    }
    m_importedGroups += remainingZones;
    m_selectedImportedIndex = 0;
    emit importedCodeplugChanged();
    emit importedSelectionChanged();
}

QVariantList RepeaterStore::slot1Talkgroups() const { return m_repeaters.empty() ? QVariantList() : toVariantList(selected().slot1); }
QVariantList RepeaterStore::slot2Talkgroups() const { return m_repeaters.empty() ? QVariantList() : toVariantList(selected().slot2); }
QVariantList RepeaterStore::selectedSimplexFrequencies() const
{
    QVariantList result;
    if (m_repeaters.empty())
        return result;
    for (const auto &item : selected().simplexFrequencies)
        result.append(QVariantMap{{QStringLiteral("name"), item.name},
                                  {QStringLiteral("frequency"), formatFrequency(item.hz)},
                                  {QStringLiteral("mode"), item.digital ? QStringLiteral("Digital") : QStringLiteral("Analog / other")},
                                  {QStringLiteral("colorCode"), item.digital ? QVariant(item.colorCode) : QVariant()},
                                  {QStringLiteral("timeSlot"), item.digital ? QVariant(item.timeSlot) : QVariant()}});
    return result;
}

QVariantList RepeaterStore::contacts() const
{
    QVariantList result;
    if (hasImportedCodeplug()) {
        for (const auto &contact : m_importedCodeplug.contacts) {
            result.append(QVariantMap{{QStringLiteral("name"), contact.name.simplified()},
                                      {QStringLiteral("id"), contact.id},
                                      {QStringLiteral("groupCall"), contact.groupCall},
                                      {QStringLiteral("index"), result.size()}});
        }
        return result;
    }
    QSet<QString> seen;
    for (const auto &contact : m_manualContacts) {
        if (contact.groupCall)
            seen.insert(QString::number(contact.id) + QLatin1Char(':') + contact.name);
        result.append(QVariantMap{{QStringLiteral("name"), contact.name},
                                  {QStringLiteral("id"), contact.id},
                                  {QStringLiteral("groupCall"), contact.groupCall},
                                  {QStringLiteral("index"), result.size()}});
    }
    for (const auto &repeater : m_repeaters) {
        for (const auto *slot : {&repeater.slot1, &repeater.slot2}) {
            for (const auto &talkgroup : *slot) {
                const QString key = QString::number(talkgroup.id) + QLatin1Char(':') + talkgroup.name;
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                result.append(QVariantMap{{QStringLiteral("name"), talkgroup.name},
                                          {QStringLiteral("id"), talkgroup.id},
                                          {QStringLiteral("groupCall"), true},
                                          {QStringLiteral("index"), result.size()}});
            }
        }
    }
    return result;
}

bool RepeaterStore::addContact(const QString &name, int id, bool groupCall)
{
    const QString cleanName = name.trimmed();
    const int nameLimit = 16;
    if (cleanName.isEmpty() || cleanName.size() > nameLimit || id < 1 || id > 16777215)
        return fail(tr("Enter a name of at most %1 characters and a DMR ID from 1 to 16777215.").arg(nameLimit));
    const QVariantList existing = contacts();
    for (const QVariant &value : existing) {
        const QVariantMap contact = value.toMap();
        if (contact.value(QStringLiteral("id")).toInt() == id)
            return fail(tr("A person or talkgroup already uses that DMR ID. Choose a different ID."));
    }
    auto &items = hasImportedCodeplug() ? m_importedCodeplug.contacts : m_manualContacts;
    if (items.size() >= 10000)
        return fail(tr("The contact list is full."));
    items.append({cleanName, id, groupCall});
    emit contactsChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::updateContact(int index, const QString &name, int id)
{
    const QVariantList existing = contacts();
    if (index < 0 || index >= existing.size())
        return fail(tr("Select a contact to edit."));
    const QVariantMap old = existing.at(index).toMap();
    const QString cleanName = name.trimmed();
    const int nameLimit = 16;
    if (cleanName.isEmpty() || cleanName.size() > nameLimit || id < 1 || id > 16777215)
        return fail(tr("Enter a name of at most %1 characters and a DMR ID from 1 to 16777215.").arg(nameLimit));
    for (int i = 0; i < existing.size(); ++i) {
        const QVariantMap other = existing.at(i).toMap();
        if (i != index && other.value(QStringLiteral("id")).toInt() == id
            && old.value(QStringLiteral("id")).toInt() != id)
            return fail(tr("A person or talkgroup already uses that DMR ID. Choose a different ID."));
    }
    if (hasImportedCodeplug()) {
        auto &contact = m_importedCodeplug.contacts[index];
        if (contact.name.simplified() == cleanName && contact.id == id)
            return true;
        if (contact.name.simplified() != cleanName)
            contact.name = cleanName;
        contact.id = id;
        emit importedSelectionChanged();
    } else {
        const int oldId = old.value(QStringLiteral("id")).toInt();
        const QString oldName = old.value(QStringLiteral("name")).toString();
        const bool groupCall = old.value(QStringLiteral("groupCall")).toBool();
        if (oldId == id && oldName == cleanName)
            return true;
        if (groupCall) {
            for (const auto &repeater : m_repeaters) {
                for (const auto *slot : {&repeater.slot1, &repeater.slot2}) {
                    const bool changingHere = std::any_of(slot->begin(), slot->end(), [&](const Talkgroup &item) {
                        return item.id == oldId && item.name == oldName;
                    });
                    if (changingHere && id != oldId && std::any_of(slot->begin(), slot->end(), [&](const Talkgroup &item) {
                            return item.id == id;
                        }))
                        return fail(tr("That DMR ID is already used in a repeater time slot."));
                }
            }
            for (auto &repeater : m_repeaters)
                for (auto *slot : {&repeater.slot1, &repeater.slot2})
                    for (auto &item : *slot)
                        if (item.id == oldId && item.name == oldName) {
                            item.name = cleanName;
                            item.id = id;
                        }
            emit talkgroupsChanged();
        }
        if (index < m_manualContacts.size()) {
            m_manualContacts[index].name = cleanName;
            m_manualContacts[index].id = id;
        } else {
            m_manualContacts.append({cleanName, id, groupCall});
        }
    }
    emit contactsChanged();
    setModified(true);
    return true;
}

void RepeaterStore::newProject()
{
    emit aboutToResetProject();
    m_repeaters.clear();
    m_selectedIndex = 0;
    m_projectPath.clear();
    m_sourceCodeplugName.clear();
    m_sourceCodeplug.clear();
    m_importedCodeplug = {};
    m_manualContacts.clear();
    m_radioName.clear();
    m_radioId = 0;
    m_importedGroups.clear();
    m_selectedImportedIndex = 0;
    m_modified = false;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    emit contactsChanged();
    emit radioSettingsChanged();
    emit importedCodeplugChanged();
    emit importedSelectionChanged();
    emit projectChanged();
}

void RepeaterStore::newProjectDialog()
{
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Creating a new project will discard your unsaved changes."))
                          != QMessageBox::Yes)
        return;
    emit aboutToResetProject();
    QTimer::singleShot(100, this, [this] { newProject(); });
}

bool RepeaterStore::addRepeater(const QString &name, const QString &rxFrequency,
                                const QString &txFrequency, int colorCode, bool highPower)
{
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0, txHz = 0;
    if (cleanName.isEmpty() || cleanName.size() > 32 || m_repeaters.size() >= 10000
        || !parseFrequency(rxFrequency, &rxHz) || !parseFrequency(txFrequency, &txHz)
        || colorCode < 0 || colorCode > 15)
        return false;
    m_repeaters.push_back({cleanName, rxHz, txHz, colorCode, {}, {}, {}, highPower});
    m_selectedIndex = static_cast<int>(m_repeaters.size()) - 1;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    setModified(true);
    return true;
}

void RepeaterStore::selectRepeater(int index)
{
    if (index < 0 || index >= static_cast<int>(m_repeaters.size()) || index == m_selectedIndex)
        return;
    m_selectedIndex = index;
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
}

bool RepeaterStore::addSimplexToSelectedRepeater(const QString &name, const QString &frequency,
                                                 bool digital, int colorCode, int timeSlot)
{
    qint64 hz = 0;
    const QString cleanName = name.trimmed();
    if (m_repeaters.empty() || cleanName.isEmpty() || cleanName.size() > 16
        || selected().simplexFrequencies.size() >= 64 || !parseFrequency(frequency, &hz)
        || (digital && (colorCode < 0 || colorCode > 15 || timeSlot < 1 || timeSlot > 2)))
        return false;
    selected().simplexFrequencies.push_back({cleanName, hz, digital,
                                             digital ? colorCode : 0, digital ? timeSlot : 0});
    emit simplexFrequenciesChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::removeSimplexFromSelectedRepeater(int index)
{
    if (m_repeaters.empty() || index < 0
        || index >= static_cast<int>(selected().simplexFrequencies.size()))
        return false;
    auto &frequencies = selected().simplexFrequencies;
    frequencies.erase(frequencies.begin() + index);
    emit simplexFrequenciesChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::updateSelectedRepeater(const QString &name, const QString &rxFrequency,
                                            const QString &txFrequency, int colorCode,
                                            bool highPower)
{
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0;
    qint64 txHz = 0;
    if (m_repeaters.empty() || cleanName.isEmpty() || cleanName.size() > 32 || !parseFrequency(rxFrequency, &rxHz)
        || !parseFrequency(txFrequency, &txHz) || colorCode < 0 || colorCode > 15)
        return false;

    auto &repeater = selected();
    if (repeater.name == cleanName && repeater.rxHz == rxHz && repeater.txHz == txHz
        && repeater.colorCode == colorCode && repeater.highPower == highPower)
        return true;
    repeater.name = cleanName;
    repeater.rxHz = rxHz;
    repeater.txHz = txHz;
    repeater.colorCode = colorCode;
    repeater.highPower = highPower;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addTalkgroup(int timeSlot, const QString &name, int id,
                                 const QString &channelName)
{
    const QString cleanName = name.trimmed();
    const QString cleanChannelName = channelName.trimmed().isEmpty() ? cleanName : channelName.trimmed();
    if (hasImportedCodeplug() || m_repeaters.empty() || (timeSlot != 1 && timeSlot != 2)
        || cleanName.isEmpty() || cleanName.size() > 32 || cleanChannelName.isEmpty()
        || cleanChannelName.size() > 16 || id < 1 || id > 16777215
        || availableChannelName(cleanChannelName) != cleanChannelName)
        return false;

    auto &items = talkgroups(timeSlot);
    if (std::any_of(items.begin(), items.end(), [id](const Talkgroup &item) { return item.id == id; }))
        return false;
    for (const QVariant &value : contacts()) {
        const QVariantMap contact = value.toMap();
        if (contact.value(QStringLiteral("id")).toInt() == id
            && (!contact.value(QStringLiteral("groupCall")).toBool()
                || contact.value(QStringLiteral("name")).toString().simplified() != cleanName.simplified()))
            return false;
    }
    const bool inLibrary = std::any_of(m_manualContacts.cbegin(), m_manualContacts.cend(),
                                       [id](const ImportedContact &contact) { return contact.id == id; });
    if (!inLibrary) {
        if (m_manualContacts.size() >= 10000)
            return false;
        m_manualContacts.append({cleanName, id, true});
    }
    items.push_back({cleanName, id, cleanChannelName});
    emit talkgroupsChanged();
    emit contactsChanged();
    setModified(true);
    return true;
}

void RepeaterStore::removeTalkgroup(int timeSlot, int id)
{
    if (m_repeaters.empty() || (timeSlot != 1 && timeSlot != 2))
        return;
    auto &items = talkgroups(timeSlot);
    const auto firstRemoved = std::remove_if(items.begin(), items.end(),
                                             [id](const Talkgroup &item) { return item.id == id; });
    if (firstRemoved == items.end())
        return;
    items.erase(firstRemoved, items.end());
    emit talkgroupsChanged();
    emit contactsChanged();
    setModified(true);
}

bool RepeaterStore::saveProjectToFile(const QString &path)
{
    QJsonArray repeaters;
    for (const auto &repeater : m_repeaters) {
        auto convert = [](const std::vector<Talkgroup> &groups) {
            QJsonArray result;
            for (const auto &group : groups) {
                QJsonObject item{{QStringLiteral("name"), group.name},
                                 {QStringLiteral("id"), group.id}};
                if (!group.channelName.isEmpty()) {
                    item.insert(QStringLiteral("channelName"), group.channelName);
                }
                result.append(item);
            }
            return result;
        };
        QJsonObject item{
            {QStringLiteral("name"), repeater.name},
            {QStringLiteral("rxHz"), repeater.rxHz},
            {QStringLiteral("txHz"), repeater.txHz},
            {QStringLiteral("colorCode"), repeater.colorCode},
            {QStringLiteral("highPower"), repeater.highPower},
            {QStringLiteral("slots"), QJsonObject{{QStringLiteral("1"), convert(repeater.slot1)},
                                                   {QStringLiteral("2"), convert(repeater.slot2)}}}};
        if (!repeater.simplexFrequencies.empty()) {
            QJsonArray simplex;
            for (const auto &frequency : repeater.simplexFrequencies)
                simplex.append(QJsonObject{{QStringLiteral("name"), frequency.name},
                                           {QStringLiteral("frequencyHz"), frequency.hz},
                                           {QStringLiteral("digital"), frequency.digital},
                                           {QStringLiteral("colorCode"), frequency.colorCode},
                                           {QStringLiteral("timeSlot"), frequency.timeSlot}});
            item.insert(QStringLiteral("simplexFrequencies"), simplex);
        }
        repeaters.append(item);
    }

    QJsonObject root{{QStringLiteral("format"), projectFormat},
                     {QStringLiteral("schemaVersion"), 6},
                     {QStringLiteral("repeaters"), repeaters}};
    root.insert(QStringLiteral("radioSettings"),
                QJsonObject{{QStringLiteral("name"), m_radioName},
                            {QStringLiteral("id"), m_radioId}});
    if (!m_manualContacts.isEmpty()) {
        QJsonArray library;
        for (const auto &contact : m_manualContacts)
            library.append(QJsonObject{{QStringLiteral("name"), contact.name},
                                       {QStringLiteral("id"), contact.id},
                                       {QStringLiteral("groupCall"), contact.groupCall}});
        root.insert(QStringLiteral("contactLibrary"), library);
    }
    if (hasImportedCodeplug()) {
        QJsonArray channels;
        for (const auto &channel : m_importedCodeplug.channels)
            channels.append(QJsonObject{{QStringLiteral("name"), channel.name},
                                        {QStringLiteral("frequencyAHz"), int(channel.frequencyAHz)},
                                        {QStringLiteral("frequencyBHz"), int(channel.frequencyBHz)},
                                        {QStringLiteral("digital"), channel.digital},
                                        {QStringLiteral("colorCode"), channel.colorCode},
                                        {QStringLiteral("timeSlot"), channel.timeSlot},
                                        {QStringLiteral("contactIndex"), channel.contactIndex},
                                        {QStringLiteral("highPower"), channel.highPower.has_value()
                                             ? QJsonValue(*channel.highPower) : QJsonValue(QJsonValue::Null)}});
        QJsonArray contacts;
        for (const auto &contact : m_importedCodeplug.contacts)
            contacts.append(QJsonObject{{QStringLiteral("name"), contact.name},
                                        {QStringLiteral("id"), contact.id},
                                        {QStringLiteral("groupCall"), contact.groupCall}});
        QJsonArray zones;
        for (const auto &zone : m_importedCodeplug.zones) {
            QJsonArray indexes;
            for (int index : zone.channelIndexes)
                indexes.append(index);
            zones.append(QJsonObject{{QStringLiteral("name"), zone.name},
                                     {QStringLiteral("channelIndexes"), indexes}});
        }
        root.insert(QStringLiteral("importedCodeplug"),
                    QJsonObject{{QStringLiteral("model"), m_importedCodeplug.model},
                                {QStringLiteral("cpsVersion"), m_importedCodeplug.cpsVersion},
                                {QStringLiteral("channels"), channels},
                                {QStringLiteral("contacts"), contacts},
                                {QStringLiteral("zones"), zones}});
    }
    QMap<QString, QByteArray> entries;
    if (!m_sourceCodeplugName.isEmpty()) {
        const QByteArray hash = QCryptographicHash::hash(m_sourceCodeplug, QCryptographicHash::Sha256).toHex();
        root.insert(QStringLiteral("sourceCodeplug"),
                    QJsonObject{{QStringLiteral("entry"), sourceEntry},
                                {QStringLiteral("fileName"), m_sourceCodeplugName},
                                {QStringLiteral("size"), m_sourceCodeplug.size()},
                                {QStringLiteral("sha256"), QString::fromLatin1(hash)}});
        entries.insert(sourceEntry, m_sourceCodeplug);
    }
    entries.insert(QStringLiteral("project.json"), QJsonDocument(root).toJson(QJsonDocument::Indented));

    QString error;
    if (!ZipArchive::write(path, entries, &error))
        return fail(QStringLiteral("Could not save project: %1").arg(error));
    m_projectPath = path;
    m_modified = false;
    emit projectChanged();
    return true;
}

bool RepeaterStore::openProjectFromFile(const QString &path)
{
    QMap<QString, QByteArray> entries;
    QString error;
    if (!ZipArchive::read(path, &entries, &error))
        return fail(QStringLiteral("Could not open project: %1").arg(error));
    if (!entries.contains(QStringLiteral("project.json")))
        return fail(QStringLiteral("Project is missing project.json."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(entries.value(QStringLiteral("project.json")), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(QStringLiteral("Project JSON is invalid."));
    const QJsonObject root = document.object();
    qint64 version = 0;
    if (root.value(QStringLiteral("format")).toString() != projectFormat
        || !integerValue(root.value(QStringLiteral("schemaVersion")), 1, 6, &version))
        return fail(QStringLiteral("This WaffleCPS project version is not supported."));

    const QJsonValue repeatersValue = root.value(QStringLiteral("repeaters"));
    if (!repeatersValue.isArray() || repeatersValue.toArray().size() > 10000
        || (version == 1 && repeatersValue.toArray().isEmpty()))
        return fail(QStringLiteral("Project has no valid repeater list."));

    std::vector<Repeater> loaded;
    for (const QJsonValue &value : repeatersValue.toArray()) {
        if (!value.isObject())
            return fail(QStringLiteral("A repeater record is invalid."));
        const QJsonObject item = value.toObject();
        Repeater repeater;
        repeater.name = item.value(QStringLiteral("name")).toString().trimmed();
        qint64 colorCode = 0;
        if (repeater.name.isEmpty() || repeater.name.size() > 32
            || !integerValue(item.value(QStringLiteral("rxHz")), 1, 999999999, &repeater.rxHz)
            || !integerValue(item.value(QStringLiteral("txHz")), 1, 999999999, &repeater.txHz)
            || !integerValue(item.value(QStringLiteral("colorCode")), 0, 15, &colorCode))
            return fail(QStringLiteral("A repeater has invalid details."));
        repeater.colorCode = static_cast<int>(colorCode);
        if (version >= 5) {
            if (!item.value(QStringLiteral("highPower")).isBool())
                return fail(QStringLiteral("A repeater has invalid transmit power."));
            repeater.highPower = item.value(QStringLiteral("highPower")).toBool();
        }

        const QJsonValue slotsValue = item.value(QStringLiteral("slots"));
        if (!slotsValue.isObject())
            return fail(QStringLiteral("A repeater is missing time slots."));
        const QJsonObject slotObject = slotsValue.toObject();
        auto readGroups = [version](const QJsonValue &arrayValue, std::vector<Talkgroup> *groups) {
            if (!arrayValue.isArray() || arrayValue.toArray().size() > 10000)
                return false;
            QSet<int> ids;
            for (const QJsonValue &groupValue : arrayValue.toArray()) {
                if (!groupValue.isObject())
                    return false;
                const QJsonObject group = groupValue.toObject();
                const QString name = group.value(QStringLiteral("name")).toString().trimmed();
                const QJsonValue channelNameValue = group.value(QStringLiteral("channelName"));
                const QString channelName = channelNameValue.toString().trimmed();
                qint64 id = 0;
                if (name.isEmpty() || name.size() > 32
                    || !integerValue(group.value(QStringLiteral("id")), 1, 16777215, &id)
                    || ids.contains(static_cast<int>(id))
                    || (!channelNameValue.isUndefined()
                        && (version < 4 || !channelNameValue.isString()
                            || channelName.isEmpty() || channelName.size() > 32)))
                    return false;
                ids.insert(static_cast<int>(id));
                groups->push_back({name, static_cast<int>(id), channelName});
            }
            return true;
        };
        if (!readGroups(slotObject.value(QStringLiteral("1")), &repeater.slot1)
            || !readGroups(slotObject.value(QStringLiteral("2")), &repeater.slot2))
            return fail(QStringLiteral("A repeater has invalid talkgroups."));
        const QJsonValue simplexValue = item.value(QStringLiteral("simplexFrequencies"));
        if (!simplexValue.isUndefined()) {
            if (version < 3 || !simplexValue.isArray() || simplexValue.toArray().size() > 64)
                return fail(QStringLiteral("A repeater has invalid simplex frequencies."));
            for (const QJsonValue &entry : simplexValue.toArray()) {
                if (!entry.isObject())
                    return fail(QStringLiteral("A simplex frequency is invalid."));
                const QJsonObject frequency = entry.toObject();
                Repeater::SimplexFrequency simplex;
                simplex.name = frequency.value(QStringLiteral("name")).toString();
                qint64 colorCode = 0, timeSlot = 0;
                if (simplex.name.isEmpty() || simplex.name.size() > 16
                    || !integerValue(frequency.value(QStringLiteral("frequencyHz")), 1, 999999999, &simplex.hz)
                    || !frequency.value(QStringLiteral("digital")).isBool()
                    || !integerValue(frequency.value(QStringLiteral("colorCode")), 0, 15, &colorCode)
                    || !integerValue(frequency.value(QStringLiteral("timeSlot")), 0, 2, &timeSlot))
                    return fail(QStringLiteral("A simplex frequency has invalid details."));
                simplex.digital = frequency.value(QStringLiteral("digital")).toBool();
                simplex.colorCode = colorCode;
                simplex.timeSlot = timeSlot;
                repeater.simplexFrequencies.push_back(simplex);
            }
        }
        loaded.push_back(std::move(repeater));
    }

    QVector<ImportedContact> manualContacts;
    const QJsonValue libraryValue = root.value(QStringLiteral("contactLibrary"));
    if (!libraryValue.isUndefined()) {
        if (version < 4 || !libraryValue.isArray() || libraryValue.toArray().size() > 10000)
            return fail(QStringLiteral("Contact library is invalid."));
        QSet<QString> usedIds;
        for (const QJsonValue &value : libraryValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("A contact is invalid."));
            const QJsonObject item = value.toObject();
            ImportedContact contact;
            contact.name = item.value(QStringLiteral("name")).toString().trimmed();
            qint64 id = 0;
            if (contact.name.isEmpty() || contact.name.size() > 32
                || !integerValue(item.value(QStringLiteral("id")), 1, 16777215, &id)
                || !item.value(QStringLiteral("groupCall")).isBool())
                return fail(QStringLiteral("A contact has invalid details."));
            contact.id = static_cast<int>(id);
            contact.groupCall = item.value(QStringLiteral("groupCall")).toBool();
            const QString key = QString::number(contact.id);
            if (usedIds.contains(key))
                return fail(QStringLiteral("The contact library has duplicate DMR IDs."));
            usedIds.insert(key);
            manualContacts.append(contact);
        }
    }

    ImportedCodeplug imported;
    if (version == 2 || (version >= 3 && root.contains(QStringLiteral("importedCodeplug")))) {
        const QJsonValue importedValue = root.value(QStringLiteral("importedCodeplug"));
        if (!importedValue.isObject())
            return fail(QStringLiteral("Imported codeplug data is missing."));
        const QJsonObject data = importedValue.toObject();
        imported.model = data.value(QStringLiteral("model")).toString();
        imported.cpsVersion = data.value(QStringLiteral("cpsVersion")).toString();
        const QJsonValue channelsValue = data.value(QStringLiteral("channels"));
        const QJsonValue zonesValue = data.value(QStringLiteral("zones"));
        if (imported.model != QStringLiteral("CS7000-M17 PLUS")
            || (imported.cpsVersion != QStringLiteral("V1.02.20.00")
                && imported.cpsVersion != QStringLiteral("V1.02.21.00"))
            || !channelsValue.isArray() || channelsValue.toArray().isEmpty()
            || channelsValue.toArray().size() > 3000 || !zonesValue.isArray()
            || zonesValue.toArray().isEmpty() || zonesValue.toArray().size() > 500)
            return fail(QStringLiteral("Imported codeplug metadata is invalid."));
        const QJsonValue contactsValue = data.value(QStringLiteral("contacts"));
        if (!contactsValue.isUndefined()) {
            if (version < 3 || !contactsValue.isArray() || contactsValue.toArray().size() > 10000)
                return fail(QStringLiteral("Imported contacts are invalid."));
            for (const QJsonValue &value : contactsValue.toArray()) {
                if (!value.isObject())
                    return fail(QStringLiteral("An imported contact is invalid."));
                const QJsonObject item = value.toObject();
                ImportedContact contact;
                contact.name = item.value(QStringLiteral("name")).toString();
                qint64 id = 0;
                if (contact.name.isEmpty() || contact.name.size() > 16
                    || !integerValue(item.value(QStringLiteral("id")), 1, 16777215, &id)
                    || !item.value(QStringLiteral("groupCall")).isBool())
                    return fail(QStringLiteral("An imported contact has invalid details."));
                contact.id = static_cast<int>(id);
                contact.groupCall = item.value(QStringLiteral("groupCall")).toBool();
                imported.contacts.append(contact);
            }
        }
        for (const QJsonValue &value : channelsValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("An imported channel is invalid."));
            const QJsonObject item = value.toObject();
            ImportedChannel channel;
            channel.name = item.value(QStringLiteral("name")).toString();
            const QJsonValue powerValue = item.value(QStringLiteral("highPower"));
            qint64 frequencyA = 0, frequencyB = 0, colorCode = 0, timeSlot = 0, contactIndex = 0;
            if (channel.name.isEmpty() || channel.name.size() > 16
                || !integerValue(item.value(QStringLiteral("frequencyAHz")), 100000000, 999999999, &frequencyA)
                || !integerValue(item.value(QStringLiteral("frequencyBHz")), 100000000, 999999999, &frequencyB)
                || !item.value(QStringLiteral("digital")).isBool()
                || !integerValue(item.value(QStringLiteral("colorCode")), 0, 15, &colorCode)
                || !integerValue(item.value(QStringLiteral("timeSlot")), 0, 2, &timeSlot)
                || (!item.value(QStringLiteral("contactIndex")).isUndefined()
                    && !integerValue(item.value(QStringLiteral("contactIndex")), 0, imported.contacts.size(), &contactIndex))
                || (version >= 5 && !powerValue.isBool() && !powerValue.isNull()))
                return fail(QStringLiteral("An imported channel has invalid details."));
            channel.frequencyAHz = frequencyA;
            channel.frequencyBHz = frequencyB;
            channel.digital = item.value(QStringLiteral("digital")).toBool();
            channel.colorCode = colorCode;
            channel.timeSlot = timeSlot;
            channel.contactIndex = contactIndex;
            if (powerValue.isBool())
                channel.highPower = powerValue.toBool();
            imported.channels.append(channel);
        }
        for (const QJsonValue &value : zonesValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("An imported zone is invalid."));
            const QJsonObject item = value.toObject();
            ImportedZone zone;
            zone.name = item.value(QStringLiteral("name")).toString();
            const QJsonValue indexesValue = item.value(QStringLiteral("channelIndexes"));
            if (zone.name.isEmpty() || zone.name.size() > 16 || !indexesValue.isArray()
                || indexesValue.toArray().size() > 16)
                return fail(QStringLiteral("An imported zone has invalid details."));
            for (const QJsonValue &indexValue : indexesValue.toArray()) {
                qint64 channelIndex = 0;
                if (!integerValue(indexValue, 0, imported.channels.size() - 1, &channelIndex))
                    return fail(QStringLiteral("An imported zone references an unknown channel."));
                zone.channelIndexes.append(channelIndex);
            }
            imported.zones.append(std::move(zone));
        }
    }

    QString sourceName;
    QByteArray sourceBytes;
    const QJsonValue sourceValue = root.value(QStringLiteral("sourceCodeplug"));
    if (!sourceValue.isUndefined()) {
        if (!sourceValue.isObject())
            return fail(QStringLiteral("Source codeplug metadata is invalid."));
        const QJsonObject source = sourceValue.toObject();
        sourceName = source.value(QStringLiteral("fileName")).toString();
        qint64 size = 0;
        if (source.value(QStringLiteral("entry")).toString() != sourceEntry
            || sourceName.isEmpty() || QFileInfo(sourceName).fileName() != sourceName
            || !integerValue(source.value(QStringLiteral("size")), 0, maximumSourceSize, &size)
            || !entries.contains(sourceEntry))
            return fail(QStringLiteral("Source codeplug is missing or invalid."));
        sourceBytes = entries.value(sourceEntry);
        const QString actualHash = QString::fromLatin1(
            QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256).toHex());
        if (sourceBytes.size() != size || source.value(QStringLiteral("sha256")).toString() != actualHash)
            return fail(QStringLiteral("Source codeplug failed its integrity check."));
    }
    if (entries.size() != (sourceName.isEmpty() ? 1 : 2))
        return fail(QStringLiteral("Project contains unsupported archive entries."));

    QString loadedRadioName;
    int loadedRadioId = 0;
    if (version >= 6) {
        const QJsonValue settingsValue = root.value(QStringLiteral("radioSettings"));
        if (!settingsValue.isObject())
            return fail(QStringLiteral("Radio settings are invalid."));
        const QJsonObject settings = settingsValue.toObject();
        loadedRadioName = settings.value(QStringLiteral("name")).toString();
        qint64 id = 0;
        if (!settings.value(QStringLiteral("name")).isString()
            || loadedRadioName.size() > 16
            || !integerValue(settings.value(QStringLiteral("id")), 0, 16777215, &id)
            || (loadedRadioName.isEmpty() != (id == 0)))
            return fail(QStringLiteral("Radio settings are invalid."));
        loadedRadioId = int(id);
    } else if (!sourceBytes.isEmpty()) {
        ImportedCodeplug sourceParsed;
        QString sourceError;
        if (CodeplugReader::read(sourceBytes, &sourceParsed, &sourceError)) {
            loadedRadioName = sourceParsed.radioName;
            loadedRadioId = sourceParsed.radioId;
        }
    }

    // Recover older project details from the untouched source when available.
    if (!imported.channels.isEmpty() && !sourceBytes.isEmpty()) {
        ImportedCodeplug sourceParsed;
        QString sourceError;
        if (CodeplugReader::read(sourceBytes, &sourceParsed, &sourceError)) {
            QMap<QString, int> sourceByName;
            for (int index = 0; index < sourceParsed.channels.size(); ++index) {
                const QString &name = sourceParsed.channels.at(index).name;
                if (sourceByName.contains(name))
                    sourceByName[name] = -1;
                else
                    sourceByName.insert(name, index);
            }
            for (int index = 0; index < imported.channels.size(); ++index) {
                auto &channel = imported.channels[index];
                if (channel.highPower.has_value())
                    continue;
                int sourceIndex = index < sourceParsed.channels.size()
                                      && sourceParsed.channels.at(index).name == channel.name
                    ? index : sourceByName.value(channel.name, -1);
                if (sourceIndex >= 0)
                    channel.highPower = sourceParsed.channels.at(sourceIndex).highPower;
            }
            if (sourceParsed.channels.size() <= imported.channels.size()) {
                if (imported.contacts.isEmpty()) {
                    imported.contacts = sourceParsed.contacts;
                    for (int index = 0; index < sourceParsed.channels.size(); ++index)
                        imported.channels[index].contactIndex = sourceParsed.channels.at(index).contactIndex;
                } else {
                    const int count = std::min(imported.contacts.size(), sourceParsed.contacts.size());
                    for (int index = 0; index < count; ++index) {
                        auto &saved = imported.contacts[index];
                        const auto &original = sourceParsed.contacts.at(index);
                        if (saved.id == original.id && saved.groupCall == original.groupCall
                            && saved.name == original.name.left(11).trimmed())
                            saved.name = original.name;
                    }
                }
            }
        }
    }

    m_repeaters = std::move(loaded);
    m_selectedIndex = 0;
    m_projectPath = path;
    m_sourceCodeplugName = sourceName;
    m_sourceCodeplug = sourceBytes;
    m_importedCodeplug = std::move(imported);
    m_manualContacts = std::move(manualContacts);
    m_radioName = loadedRadioName;
    m_radioId = loadedRadioId;
    rebuildImportedGroups();
    m_modified = false;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    emit contactsChanged();
    emit radioSettingsChanged();
    emit projectChanged();
    return true;
}

bool RepeaterStore::attachSourceCodeplugFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not read codeplug: %1").arg(file.errorString()));
    if (file.size() > maximumSourceSize)
        return fail(QStringLiteral("Codeplug exceeds the project size limit."));
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
        return fail(QStringLiteral("Could not read the complete codeplug."));
    m_sourceCodeplugName = QFileInfo(path).fileName();
    m_sourceCodeplug = bytes;
    setModified(true);
    emit projectChanged();
    return true;
}

bool RepeaterStore::importCodeplugFromBytes(const QByteArray &bytes)
{
    ImportedCodeplug imported;
    QString error;
    if (!CodeplugReader::read(bytes, &imported, &error))
        return fail(QStringLiteral("Could not import codeplug: %1").arg(error));
    m_importedCodeplug = std::move(imported);
    m_radioName = m_importedCodeplug.radioName;
    m_radioId = m_importedCodeplug.radioId;
    rebuildImportedGroups();
    emit contactsChanged();
    emit radioSettingsChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::importCodeplugFromFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not read codeplug: %1").arg(file.errorString()));
    if (file.size() > maximumSourceSize)
        return fail(QStringLiteral("Codeplug exceeds the project size limit."));
    const QByteArray bytes = file.readAll();
    if (bytes.size() != file.size())
        return fail(QStringLiteral("Could not read the complete codeplug."));
    if (!importCodeplugFromBytes(bytes))
        return false;
    m_sourceCodeplugName = QFileInfo(path).fileName();
    m_sourceCodeplug = bytes;
    m_projectPath.clear();
    emit projectChanged();
    return true;
}

bool RepeaterStore::importRadioCodeplug(const QByteArray &bytes)
{
    // Validate before replacing any part of the currently open project.
    ImportedCodeplug imported;
    QString error;
    if (!CodeplugReader::read(bytes, &imported, &error))
        return fail(tr("Could not import radio codeplug: %1").arg(error));
    emit aboutToResetProject();
    m_repeaters.clear();
    m_manualContacts.clear();
    m_selectedIndex = 0;
    m_projectPath.clear();
    m_sourceCodeplug = bytes;
    m_sourceCodeplugName = QStringLiteral("radio-read.rdbx");
    m_importedCodeplug = std::move(imported);
    m_radioName = m_importedCodeplug.radioName;
    m_radioId = m_importedCodeplug.radioId;
    rebuildImportedGroups();
    setModified(true);
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
    emit simplexFrequenciesChanged();
    emit contactsChanged();
    emit radioSettingsChanged();
    emit projectChanged();
    return true;
}

void RepeaterStore::importAttachedCodeplug()
{
    if (m_sourceCodeplug.isEmpty()) {
        fail(QStringLiteral("No source codeplug is attached."));
        return;
    }
    importCodeplugFromBytes(m_sourceCodeplug);
}

void RepeaterStore::importCodeplugDialog()
{
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Importing a codeplug will replace the current imported view and discard unsaved changes."))
                          != QMessageBox::Yes)
        return;
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Import CS7000 codeplug"), QString(),
                                                      tr("CS7000 codeplugs (*.rdbx);;All files (*)"));
    if (!path.isEmpty())
        importCodeplugFromFile(path);
}

void RepeaterStore::openProjectDialog()
{
    if (m_modified && QMessageBox::question(nullptr, tr("Discard unsaved changes?"),
                                             tr("Opening another project will discard your unsaved changes."))
                          != QMessageBox::Yes)
        return;
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Open WaffleCPS project"), QString(),
                                                      tr("WaffleCPS projects (*.waffle *.zip)"));
    if (!path.isEmpty())
        openProjectFromFile(path);
}

void RepeaterStore::saveProjectDialog(bool saveAs)
{
    QString path = saveAs ? QString() : m_projectPath;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(nullptr, tr("Save WaffleCPS project"),
                                            m_projectPath.isEmpty() ? QStringLiteral("project.waffle") : m_projectPath,
                                            tr("WaffleCPS projects (*.waffle)"));
        if (path.isEmpty())
            return;
        if (!path.endsWith(QStringLiteral(".waffle"), Qt::CaseInsensitive))
            path += QStringLiteral(".waffle");
    }
    saveProjectToFile(path);
}

bool RepeaterStore::exportCodeplugToFile(const QString &path)
{
    if (!m_sourceCodeplugName.isEmpty()
        && QFileInfo(path).fileName().compare(m_sourceCodeplugName, Qt::CaseInsensitive) == 0)
        return fail(QStringLiteral("Choose a different filename so the original codeplug stays untouched."));
    const QByteArray source = hasImportedCodeplug() ? m_sourceCodeplug : cs7000DefaultCodeplug();
    if (source.isEmpty())
        return fail(QStringLiteral("The CS7000 export base is unavailable."));
    ImportedCodeplug target;
    if (hasImportedCodeplug()) {
        target = m_importedCodeplug;
    } else {
        QString readError;
        if (!CodeplugReader::read(source, &target, &readError))
            return fail(QStringLiteral("Could not read the embedded CS7000 default: %1").arg(readError));
        if (!m_repeaters.empty()) {
            target.channels.clear();
            target.contacts = m_manualContacts;
            target.zones.clear();
            QMap<int, int> contactById;
            for (int i = 0; i < target.contacts.size(); ++i)
                contactById.insert(target.contacts.at(i).id, i + 1);
            auto contactFor = [&](const Talkgroup &group) {
                const int existing = contactById.value(group.id, 0);
                if (existing)
                    return target.contacts.at(existing - 1).groupCall ? existing : 0;
                target.contacts.append({group.name, group.id, true});
                const int index = target.contacts.size();
                contactById.insert(group.id, index);
                return index;
            };
            for (const Repeater &repeater : m_repeaters) {
                ImportedZone zone;
                zone.name = repeater.name;
                auto addGroup = [&](const Talkgroup &group, int timeSlot) {
                    const int contact = contactFor(group);
                    if (contact == 0)
                        return false;
                    ImportedChannel channel;
                    channel.name = group.channelName.isEmpty() ? group.name : group.channelName;
                    channel.frequencyAHz = quint32(repeater.txHz);
                    channel.frequencyBHz = quint32(repeater.rxHz);
                    channel.digital = true;
                    channel.colorCode = repeater.colorCode;
                    channel.timeSlot = timeSlot;
                    channel.contactIndex = contact;
                    channel.highPower = repeater.highPower;
                    zone.channelIndexes.append(target.channels.size());
                    target.channels.append(channel);
                    return true;
                };
                // New manual zones default to TS2 first, then TS1.
                for (const Talkgroup &group : repeater.slot2)
                    if (!addGroup(group, 2))
                        return fail(QStringLiteral("A talkgroup ID conflicts with a person contact."));
                for (const Talkgroup &group : repeater.slot1)
                    if (!addGroup(group, 1))
                        return fail(QStringLiteral("A talkgroup ID conflicts with a person contact."));
                for (const auto &simplex : repeater.simplexFrequencies) {
                    ImportedChannel channel;
                    channel.name = simplex.name;
                    channel.frequencyAHz = quint32(simplex.hz);
                    channel.frequencyBHz = quint32(simplex.hz);
                    channel.digital = simplex.digital;
                    channel.colorCode = simplex.digital ? simplex.colorCode : 0;
                    channel.timeSlot = simplex.digital ? simplex.timeSlot : 0;
                    channel.highPower = true;
                    zone.channelIndexes.append(target.channels.size());
                    target.channels.append(channel);
                }
                target.zones.append(std::move(zone));
            }
        } else if (!m_manualContacts.isEmpty()) {
            target.contacts += m_manualContacts;
        }
        makeExportChannelNamesUnique(&target);
    }
    QByteArray bytes;
    QString writeError;
    if (!CodeplugWriter::write(source, target, m_radioName, m_radioId, &bytes, &writeError))
        return fail(QStringLiteral("Could not export codeplug: %1").arg(writeError));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(QStringLiteral("Could not write codeplug: %1").arg(file.errorString()));
    return true;
}

void RepeaterStore::exportCodeplugDialog()
{
    QString path = QFileDialog::getSaveFileName(nullptr, tr("Export CS7000 codeplug"),
                                                 QStringLiteral("wafflecps-export.rdbx"),
                                                 tr("CS7000 codeplugs (*.rdbx)"));
    if (path.isEmpty())
        return;
    if (!path.endsWith(QStringLiteral(".rdbx"), Qt::CaseInsensitive))
        path += QStringLiteral(".rdbx");
    exportCodeplugToFile(path);
}

void RepeaterStore::attachSourceDialog()
{
    const QString path = QFileDialog::getOpenFileName(nullptr, tr("Attach original codeplug"), QString(),
                                                      tr("Codeplug files (*.rdbx *.rbdx);;All files (*)"));
    if (!path.isEmpty())
        attachSourceCodeplugFromFile(path);
}

const RepeaterStore::Repeater &RepeaterStore::selected() const
{
    return m_repeaters.at(static_cast<std::size_t>(m_selectedIndex));
}

RepeaterStore::Repeater &RepeaterStore::selected()
{
    return m_repeaters.at(static_cast<std::size_t>(m_selectedIndex));
}

std::vector<RepeaterStore::Talkgroup> &RepeaterStore::talkgroups(int timeSlot)
{
    return timeSlot == 1 ? selected().slot1 : selected().slot2;
}

QVariantList RepeaterStore::toVariantList(const std::vector<Talkgroup> &talkgroups)
{
    QVariantList result;
    for (const auto &talkgroup : talkgroups) {
        const QString channelName = talkgroup.channelName.isEmpty() ? talkgroup.name : talkgroup.channelName;
        result.append(QVariantMap{{QStringLiteral("name"), talkgroup.name},
                                  {QStringLiteral("id"), talkgroup.id},
                                  {QStringLiteral("groupCall"), true},
                                  {QStringLiteral("channelName"), channelName},
                                  {QStringLiteral("channels"), QVariantList{QVariantMap{{QStringLiteral("name"), channelName}}}}});
    }
    return result;
}

QString RepeaterStore::formatFrequency(qint64 hz)
{
    QString fraction = QStringLiteral("%1").arg(hz % 1000000, 6, 10, QChar('0'));
    while (fraction.size() > 4 && fraction.endsWith(QLatin1Char('0')))
        fraction.chop(1);
    return QString::number(hz / 1000000) + QLatin1Char('.') + fraction;
}

bool RepeaterStore::parseFrequency(const QString &text, qint64 *hz)
{
    static const QRegularExpression pattern(QStringLiteral("^(\\d{1,3})(?:\\.(\\d{1,6}))?$"));
    const auto match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return false;
    QString fraction = match.captured(2);
    fraction = fraction.leftJustified(6, QLatin1Char('0'));
    *hz = match.captured(1).toLongLong() * 1000000 + fraction.toLongLong();
    return *hz > 0;
}

void RepeaterStore::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit projectChanged();
}

bool RepeaterStore::fail(const QString &message)
{
    emit errorOccurred(message);
    return false;
}
