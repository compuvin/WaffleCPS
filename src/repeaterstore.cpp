#include "repeaterstore.h"
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

} // namespace

RepeaterStore::RepeaterStore(QObject *parent)
    : QObject(parent)
    , m_repeaters{
          {QStringLiteral("North Ridge"), 444500000, 449500000, 1,
           {{QStringLiteral("Local"), 9}, {QStringLiteral("Statewide"), 123}},
           {{QStringLiteral("Regional"), 3100}}},
          {QStringLiteral("Riverbend"), 442125000, 447125000, 3,
           {{QStringLiteral("Local"), 9}},
           {{QStringLiteral("Worldwide"), 91}, {QStringLiteral("Regional"), 3100}}}}
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
QString RepeaterStore::selectedName() const { return selected().name; }
QString RepeaterStore::rxFrequency() const { return formatFrequency(selected().rxHz); }
QString RepeaterStore::txFrequency() const { return formatFrequency(selected().txHz); }
int RepeaterStore::colorCode() const { return selected().colorCode; }
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
    for (int zoneIndex : group.zoneIndexes) {
        const auto &zone = m_importedCodeplug.zones.at(zoneIndex);
        QVariantList channels;
        for (int channelIndex : zone.channelIndexes) {
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            channels.append(QVariantMap{
                {QStringLiteral("name"), channel.name},
                {QStringLiteral("number"), channelIndex + 1},
                {QStringLiteral("frequencyA"), formatFrequency(channel.frequencyAHz)},
                {QStringLiteral("frequencyB"), formatFrequency(channel.frequencyBHz)},
                {QStringLiteral("mode"), channel.digital ? QStringLiteral("Digital") : QStringLiteral("Analog / other")},
                {QStringLiteral("colorCode"), channel.digital ? QVariant(channel.colorCode) : QVariant()},
                {QStringLiteral("timeSlot"), channel.digital ? QVariant(channel.timeSlot) : QVariant()}});
        }
        zones.append(QVariantMap{{QStringLiteral("name"), zone.name},
                                 {QStringLiteral("channels"), channels}});
    }
    return {{QStringLiteral("name"), group.name},
            {QStringLiteral("classification"), group.reason},
            {QStringLiteral("zones"), zones}};
}

void RepeaterStore::selectImportedGroup(int index)
{
    if (index < 0 || index >= m_importedGroups.size() || index == m_selectedImportedIndex)
        return;
    m_selectedImportedIndex = index;
    emit importedSelectionChanged();
}

void RepeaterStore::rebuildImportedGroups()
{
    m_importedGroups.clear();
    QMap<QString, int> repeaterIndexes;
    QVector<ImportedGroup> remainingZones;
    for (int zoneIndex = 0; zoneIndex < m_importedCodeplug.zones.size(); ++zoneIndex) {
        const auto &zone = m_importedCodeplug.zones.at(zoneIndex);
        bool repeater = !zone.channelIndexes.isEmpty();
        QString reason = repeater ? QString() : QStringLiteral("Empty zone");
        ImportedChannel first;
        if (repeater)
            first = m_importedCodeplug.channels.at(zone.channelIndexes.constFirst());
        if (repeater && !first.digital) {
            repeater = false;
            reason = QStringLiteral("Contains an analog or unrecognized channel");
        }
        if (repeater && first.frequencyAHz == first.frequencyBHz) {
            repeater = false;
            reason = QStringLiteral("Same-frequency digital channels may be simplex or a hotspot");
        }
        for (int channelIndex : zone.channelIndexes) {
            const auto &channel = m_importedCodeplug.channels.at(channelIndex);
            if (!channel.digital) {
                repeater = false;
                reason = QStringLiteral("Contains an analog or unrecognized channel");
            } else if (channel.frequencyAHz != first.frequencyAHz
                       || channel.frequencyBHz != first.frequencyBHz) {
                repeater = false;
                reason = QStringLiteral("Channels use different frequency pairs");
            } else if (channel.colorCode != first.colorCode) {
                repeater = false;
                reason = QStringLiteral("Channels use different color codes");
            }
        }
        if (repeater) {
            const QString key = QStringLiteral("%1/%2/%3")
                .arg(first.frequencyAHz).arg(first.frequencyBHz).arg(first.colorCode);
            if (repeaterIndexes.contains(key)) {
                m_importedGroups[repeaterIndexes.value(key)].zoneIndexes.append(zoneIndex);
            } else {
                ImportedGroup group;
                group.name = zone.name;
                group.reason = QStringLiteral("All channels share the same non-simplex frequency pair and color code");
                group.zoneIndexes.append(zoneIndex);
                group.repeater = true;
                group.frequencyAHz = first.frequencyAHz;
                group.frequencyBHz = first.frequencyBHz;
                group.colorCode = first.colorCode;
                repeaterIndexes.insert(key, m_importedGroups.size());
                m_importedGroups.append(group);
            }
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

QVariantList RepeaterStore::slot1Talkgroups() const { return toVariantList(selected().slot1); }
QVariantList RepeaterStore::slot2Talkgroups() const { return toVariantList(selected().slot2); }

void RepeaterStore::selectRepeater(int index)
{
    if (index < 0 || index >= static_cast<int>(m_repeaters.size()) || index == m_selectedIndex)
        return;
    m_selectedIndex = index;
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
}

bool RepeaterStore::updateSelectedRepeater(const QString &name, const QString &rxFrequency,
                                            const QString &txFrequency, int colorCode)
{
    const QString cleanName = name.trimmed();
    qint64 rxHz = 0;
    qint64 txHz = 0;
    if (cleanName.isEmpty() || cleanName.size() > 32 || !parseFrequency(rxFrequency, &rxHz)
        || !parseFrequency(txFrequency, &txHz) || colorCode < 0 || colorCode > 15)
        return false;

    auto &repeater = selected();
    if (repeater.name == cleanName && repeater.rxHz == rxHz && repeater.txHz == txHz
        && repeater.colorCode == colorCode)
        return true;
    repeater.name = cleanName;
    repeater.rxHz = rxHz;
    repeater.txHz = txHz;
    repeater.colorCode = colorCode;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    setModified(true);
    return true;
}

bool RepeaterStore::addTalkgroup(int timeSlot, const QString &name, int id)
{
    const QString cleanName = name.trimmed();
    if ((timeSlot != 1 && timeSlot != 2) || cleanName.isEmpty() || cleanName.size() > 32
        || id < 1 || id > 16777215)
        return false;

    auto &items = talkgroups(timeSlot);
    if (std::any_of(items.begin(), items.end(), [id](const Talkgroup &item) { return item.id == id; }))
        return false;
    items.push_back({cleanName, id});
    emit talkgroupsChanged();
    setModified(true);
    return true;
}

void RepeaterStore::removeTalkgroup(int timeSlot, int id)
{
    if (timeSlot != 1 && timeSlot != 2)
        return;
    auto &items = talkgroups(timeSlot);
    const auto firstRemoved = std::remove_if(items.begin(), items.end(),
                                             [id](const Talkgroup &item) { return item.id == id; });
    if (firstRemoved == items.end())
        return;
    items.erase(firstRemoved, items.end());
    emit talkgroupsChanged();
    setModified(true);
}

bool RepeaterStore::saveProjectToFile(const QString &path)
{
    QJsonArray repeaters;
    for (const auto &repeater : m_repeaters) {
        auto convert = [](const std::vector<Talkgroup> &groups) {
            QJsonArray result;
            for (const auto &group : groups)
                result.append(QJsonObject{{QStringLiteral("name"), group.name},
                                          {QStringLiteral("id"), group.id}});
            return result;
        };
        repeaters.append(QJsonObject{
            {QStringLiteral("name"), repeater.name},
            {QStringLiteral("rxHz"), repeater.rxHz},
            {QStringLiteral("txHz"), repeater.txHz},
            {QStringLiteral("colorCode"), repeater.colorCode},
            {QStringLiteral("slots"), QJsonObject{{QStringLiteral("1"), convert(repeater.slot1)},
                                                   {QStringLiteral("2"), convert(repeater.slot2)}}}});
    }

    QJsonObject root{{QStringLiteral("format"), projectFormat},
                     {QStringLiteral("schemaVersion"), hasImportedCodeplug() ? 2 : 1},
                     {QStringLiteral("repeaters"), repeaters}};
    if (hasImportedCodeplug()) {
        QJsonArray channels;
        for (const auto &channel : m_importedCodeplug.channels)
            channels.append(QJsonObject{{QStringLiteral("name"), channel.name},
                                        {QStringLiteral("frequencyAHz"), int(channel.frequencyAHz)},
                                        {QStringLiteral("frequencyBHz"), int(channel.frequencyBHz)},
                                        {QStringLiteral("digital"), channel.digital},
                                        {QStringLiteral("colorCode"), channel.colorCode},
                                        {QStringLiteral("timeSlot"), channel.timeSlot}});
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
        || !integerValue(root.value(QStringLiteral("schemaVersion")), 1, 2, &version))
        return fail(QStringLiteral("This WaffleCPS project version is not supported."));

    const QJsonValue repeatersValue = root.value(QStringLiteral("repeaters"));
    if (!repeatersValue.isArray() || repeatersValue.toArray().isEmpty()
        || repeatersValue.toArray().size() > 10000)
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

        const QJsonValue slotsValue = item.value(QStringLiteral("slots"));
        if (!slotsValue.isObject())
            return fail(QStringLiteral("A repeater is missing time slots."));
        const QJsonObject slotObject = slotsValue.toObject();
        auto readGroups = [](const QJsonValue &arrayValue, std::vector<Talkgroup> *groups) {
            if (!arrayValue.isArray() || arrayValue.toArray().size() > 10000)
                return false;
            QSet<int> ids;
            for (const QJsonValue &groupValue : arrayValue.toArray()) {
                if (!groupValue.isObject())
                    return false;
                const QJsonObject group = groupValue.toObject();
                const QString name = group.value(QStringLiteral("name")).toString().trimmed();
                qint64 id = 0;
                if (name.isEmpty() || name.size() > 32
                    || !integerValue(group.value(QStringLiteral("id")), 1, 16777215, &id)
                    || ids.contains(static_cast<int>(id)))
                    return false;
                ids.insert(static_cast<int>(id));
                groups->push_back({name, static_cast<int>(id)});
            }
            return true;
        };
        if (!readGroups(slotObject.value(QStringLiteral("1")), &repeater.slot1)
            || !readGroups(slotObject.value(QStringLiteral("2")), &repeater.slot2))
            return fail(QStringLiteral("A repeater has invalid talkgroups."));
        loaded.push_back(std::move(repeater));
    }

    ImportedCodeplug imported;
    if (version == 2) {
        const QJsonValue importedValue = root.value(QStringLiteral("importedCodeplug"));
        if (!importedValue.isObject())
            return fail(QStringLiteral("Imported codeplug data is missing."));
        const QJsonObject data = importedValue.toObject();
        imported.model = data.value(QStringLiteral("model")).toString();
        imported.cpsVersion = data.value(QStringLiteral("cpsVersion")).toString();
        const QJsonValue channelsValue = data.value(QStringLiteral("channels"));
        const QJsonValue zonesValue = data.value(QStringLiteral("zones"));
        if (imported.model != QStringLiteral("CS7000-M17 PLUS")
            || imported.cpsVersion != QStringLiteral("V1.02.20.00")
            || !channelsValue.isArray() || channelsValue.toArray().isEmpty()
            || channelsValue.toArray().size() > 3000 || !zonesValue.isArray()
            || zonesValue.toArray().isEmpty() || zonesValue.toArray().size() > 500)
            return fail(QStringLiteral("Imported codeplug metadata is invalid."));
        for (const QJsonValue &value : channelsValue.toArray()) {
            if (!value.isObject())
                return fail(QStringLiteral("An imported channel is invalid."));
            const QJsonObject item = value.toObject();
            ImportedChannel channel;
            channel.name = item.value(QStringLiteral("name")).toString();
            qint64 frequencyA = 0, frequencyB = 0, colorCode = 0, timeSlot = 0;
            if (channel.name.isEmpty() || channel.name.size() > 16
                || !integerValue(item.value(QStringLiteral("frequencyAHz")), 100000000, 999999999, &frequencyA)
                || !integerValue(item.value(QStringLiteral("frequencyBHz")), 100000000, 999999999, &frequencyB)
                || !item.value(QStringLiteral("digital")).isBool()
                || !integerValue(item.value(QStringLiteral("colorCode")), 0, 15, &colorCode)
                || !integerValue(item.value(QStringLiteral("timeSlot")), 0, 2, &timeSlot))
                return fail(QStringLiteral("An imported channel has invalid details."));
            channel.frequencyAHz = frequencyA;
            channel.frequencyBHz = frequencyB;
            channel.digital = item.value(QStringLiteral("digital")).toBool();
            channel.colorCode = colorCode;
            channel.timeSlot = timeSlot;
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
                || indexesValue.toArray().size() > 64)
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

    m_repeaters = std::move(loaded);
    m_selectedIndex = 0;
    m_projectPath = path;
    m_sourceCodeplugName = sourceName;
    m_sourceCodeplug = sourceBytes;
    m_importedCodeplug = std::move(imported);
    rebuildImportedGroups();
    m_modified = false;
    emit repeatersChanged();
    emit selectedRepeaterChanged();
    emit talkgroupsChanged();
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
    rebuildImportedGroups();
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
    for (const auto &talkgroup : talkgroups)
        result.append(QVariantMap{{QStringLiteral("name"), talkgroup.name},
                                  {QStringLiteral("id"), talkgroup.id}});
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
