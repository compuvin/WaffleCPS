#include "codeplugconversion.h"
#include <QJsonArray>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace {
void require(bool condition, const QString &message)
{
    if (!condition) throw std::runtime_error(message.toStdString());
}
qint64 limitNumber(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double raw = value.toDouble();
        require(std::isfinite(raw) && raw >= 0 && raw <= 4294967295.0 && std::floor(raw) == raw,
                "Invalid numeric conversion limit.");
        return qint64(raw);
    }
    bool ok = false;
    const QString text = value.toString();
    const qint64 result = text.toLongLong(&ok, text.startsWith("0x", Qt::CaseInsensitive) ? 16 : 10);
    require(ok && result >= 0 && result <= 4294967295LL, "Invalid numeric conversion limit.");
    return result;
}
QJsonObject nameRule(const QJsonObject &limits, QString key)
{
    const auto names = limits.value("names").toObject();
    for (int depth = 0; depth < 16; ++depth) {
        const auto rule = names.value(key).toObject();
        require(!rule.isEmpty(), "Missing conversion name rule: " + key);
        if (!rule.contains("same_as")) return rule;
        key = rule.value("same_as").toString();
    }
    throw std::runtime_error("Cyclic conversion name rule");
}
QString fitName(QString text, const QJsonObject &limits, const QString &key, QSet<QString> *used = nullptr)
{
    const auto rule = nameRule(limits, key);
    const bool unicode = rule.value("type") == "utf16le";
    int maximum = int(limitNumber(rule.value(unicode ? "maximum_code_units" : "maximum_characters")));
    if (rule.contains("storage_characters")) maximum = std::min(maximum, int(limitNumber(rule.value("storage_characters"))));
    require(maximum > 0, "Invalid conversion name length.");
    const QString alphabet = rule.value("allowed_characters").toString();
    const QString storageAlphabet = rule.value("storage_alphabet").toString();
    QString cleaned;
    for (QChar ch : text) {
        bool allowed = unicode ? ch.unicode() >= (rule.value("minimum_code_unit").isString() ? rule.value("minimum_code_unit").toString().toInt(nullptr, 0) : rule.value("minimum_code_unit").toInt()) : alphabet.contains(ch);
        for (const auto &bad : rule.value("forbidden_code_units").toArray()) {
            bool ok = false;
            const int value = bad.isString() ? bad.toString().toInt(&ok, 0) : bad.toInt();
            if (ch.unicode() == value) allowed = false;
        }
        if (!storageAlphabet.isEmpty()) allowed = allowed && storageAlphabet.contains(ch);
        cleaned += allowed ? ch : QChar(' ');
    }
    cleaned = cleaned.trimmed();
    if (cleaned.isEmpty()) cleaned = "Converted";
    // Sanitize the fallback as well for definitions with restricted alphabets.
    if (!unicode) for (int i = 0; i < cleaned.size(); ++i)
        if (!alphabet.contains(cleaned[i])) cleaned[i] = alphabet.contains(' ') ? QChar(' ') : (alphabet.isEmpty() ? QChar(' ') : alphabet.at(0));
    cleaned = cleaned.left(maximum);
    if (!cleaned.isEmpty() && cleaned.back().isHighSurrogate()) cleaned.chop(1);
    require(!cleaned.trimmed().isEmpty(), "Cannot form a destination name.");
    QString result = cleaned;
    for (int suffix = 2; used && used->contains(result.toCaseFolded()); ++suffix) {
        require(suffix < 1000000, "Cannot make destination names unique.");
        const QString tail = QString::number(suffix);
        require(tail.size() < maximum, "Destination name limit cannot accommodate unique names.");
        result = cleaned.left(maximum - tail.size() - 1).trimmed() + " " + tail;
    }
    if (used) used->insert(result.toCaseFolded());
    return result;
}
bool inBand(quint32 hz, const CodeplugConversion::Session &s)
{
    return (s.twoMeters && hz >= 136000000 && hz <= 174000000)
        || (s.seventyCm && hz >= 400000000 && hz <= 470000000);
}
QVariantMap choice(int index, const QString &name, bool checked, bool enabled, const QString &detail)
{
    return {{"index", index}, {"name", name}, {"checked", checked}, {"enabled", enabled}, {"detail", detail}};
}
QVector<int> members(const ImportedZone &zone, bool separate)
{
    QVector<int> result = zone.channelIndexes;
    if (separate) for (int index : zone.bottomChannelIndexes) if (!result.contains(index)) result.append(index);
    return result;
}
}

void CodeplugConversion::rebuild(Session &s, bool initialize)
{
    const auto limits = s.metadata.value("limits").toObject();
    const auto callTypes = s.metadata.value("call_types").toArray();
    const auto modes = s.metadata.value("modes").toObject();
    ImportedCodeplug target = s.baseline;
    target.channels.clear(); target.contacts.clear(); target.zones.clear();
    QStringList changes, losses, blockers;
    QVariantList contactRows, channelRows, zoneRows;
    if (!s.twoMeters && !s.seventyCm) blockers.append("Select at least one destination band.");
    QVector<int> uses(s.source.contacts.size(), 0);
    for (int i = 0; i < uses.size() && i < s.contactUses.size(); ++i) uses[i] = s.contactUses[i];
    for (const auto &channel : s.source.channels)
        if (channel.contactIndex > 0 && channel.contactIndex <= uses.size()) ++uses[channel.contactIndex - 1];
    auto contactReason = [&](int i) {
        const auto &c = s.source.contacts[i];
        const auto bound = limits.value("contact_id").toObject();
        if (c.id < limitNumber(bound.value("minimum")) || c.id > limitNumber(bound.value("maximum"))) return QString("Contact ID cannot be represented by the destination.");
        if (!callTypes.contains(c.callTypeValue())) return QString("Call type is not supported by the destination.");
        return QString();
    };
    if (initialize) {
        s.contacts.clear(); s.channels.clear(); s.zones.clear();
        QVector<int> order(s.source.contacts.size()); std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            auto rank = [&](int i) { return (uses[i] > 0 ? 0 : 2) + (s.source.contacts[i].callTypeValue() == "group" ? 0 : 1); };
            return rank(a) < rank(b);
        });
        for (int i : order)
            if (s.contacts.size() < target.contactCapacity && contactReason(i).isEmpty()) s.contacts.insert(i);
        for (int i = 0; i < s.source.zones.size(); ++i) s.zones.insert(i);
    }
    QMap<int, int> contactMap;
    QSet<QString> contactNames;
    for (int i = 0; i < s.source.contacts.size(); ++i) {
        auto c = s.source.contacts[i];
        const QString reason = contactReason(i);
        if (!reason.isEmpty()) s.contacts.remove(i);
        const bool keep = s.contacts.contains(i);
        QString detail = QString("%1 call · ID %2 · %3 known references").arg(c.callTypeValue()).arg(c.id).arg(uses[i]);
        if (!reason.isEmpty()) detail += " · " + reason;
        if (keep) {
            const QString original = c.name;
            c.name = fitName(c.name, limits, "contact_name", &contactNames);
            if (c.name != original) changes.append(QString("Contact ‘%1’ renamed to ‘%2’.").arg(original, c.name));
            c.sourceRecord = -1;
            contactMap.insert(i + 1, target.contacts.size() + 1);
            target.contacts.append(c);
        } else losses.append(QString("Contact ‘%1’ (ID %2): %3").arg(c.name).arg(c.id).arg(reason.isEmpty() ? "not selected" : reason));
        contactRows.append(choice(i, s.source.contacts[i].name, keep, reason.isEmpty(), detail));
    }
    auto channelReason = [&](const ImportedChannel &c) {
        if (!inBand(c.frequencyAHz, s) || !inBand(c.frequencyBHz, s)) return QString("Receive or transmit frequency is outside the selected bands.");
        if (!modes.contains(c.digital ? "digital" : "analog")) return QString("Channel mode is not supported by the destination.");
        const auto frequency = limits.value("frequency_hz").toObject();
        if (c.frequencyAHz < limitNumber(frequency.value("minimum")) || c.frequencyAHz > limitNumber(frequency.value("maximum"))
            || c.frequencyBHz < limitNumber(frequency.value("minimum")) || c.frequencyBHz > limitNumber(frequency.value("maximum")))
            return QString("Frequency cannot be encoded by the destination.");
        if (c.contactIndex && !contactMap.contains(c.contactIndex)) return QString("Its transmit contact is not selected or supported.");
        return QString();
    };
    QSet<QString> channelNames;
    auto adaptChannel = [&](ImportedChannel c, bool report) {
        const QString original = c.name;
        c.name = fitName(c.name, limits, "channel_name", report ? &channelNames : nullptr);
        if (report && c.name != original) changes.append(QString("Channel ‘%1’ renamed to ‘%2’.").arg(original, c.name));
        const QString power = c.powerValue();
        if (!target.powerSettings.contains(power)) {
            const QString replacement = target.powerSettings.contains("high") ? "high" : target.powerSettings.value(0);
            c.setPowerValue(replacement);
            if (report && !power.isEmpty()) changes.append(QString("Channel ‘%1’: power %2 → %3.").arg(original, power, replacement.isEmpty() ? "radio setting" : replacement));
        }
        if (c.digital) {
            c.receiveCtcss.reset();
            c.transmitCtcss.reset();
        } else {
            for (const auto &entry : {qMakePair(&c.receiveCtcss, QString("receive")), qMakePair(&c.transmitCtcss, QString("transmit"))}) {
                auto *tone = entry.first;
                if (!tone->has_value() || !target.ctcssTones.contains(tone->value())) {
                    if (report && (!tone->has_value() || tone->value() != 0))
                        changes.append(QString("Channel ‘%1’: its unsupported or unrepresented %2 tone will not transfer.").arg(original, entry.second));
                    *tone = target.ctcssTones.contains(0) ? std::optional<int>(0) : std::nullopt;
                }
            }
        }
        c.contactIndex = contactMap.value(c.contactIndex, 0);
        c.sourceRecord = -1; c.useDefaultTemplate = true;
        return c;
    };
    QMap<int, int> channelMap;
    for (int i = 0; i < s.source.channels.size(); ++i) {
        const auto &c = s.source.channels[i];
        const QString reason = channelReason(c);
        if (initialize && reason.isEmpty() && s.channels.size() < target.channelCapacity) s.channels.insert(i);
        if (!reason.isEmpty()) s.channels.remove(i);
        const bool keep = s.channels.contains(i);
        if (keep) { channelMap.insert(i, target.channels.size()); target.channels.append(adaptChannel(c, true)); }
        else losses.append(QString("Channel ‘%1’: %2").arg(c.name, reason.isEmpty() ? "not selected" : reason));
        const QString detail = QString("RX %1 · TX %2 MHz%3").arg(c.frequencyBHz / 1e6, 0, 'f', 5)
            .arg(c.frequencyAHz / 1e6, 0, 'f', 5).arg(reason.isEmpty() ? "" : " · " + reason);
        channelRows.append(choice(i, c.name, keep, reason.isEmpty(), detail));
    }
    QSet<QString> zoneNames;
    const int perZone = target.zoneMemberCapacity;
    require(perZone > 0, "Invalid destination zone member limit.");
    auto remap = [&](const QVector<int> &list) {
        QVector<int> result;
        for (int i : list) if (channelMap.contains(i) && !result.contains(channelMap[i])) result.append(channelMap[i]);
        return result;
    };
    for (int i = 0; i < s.source.zones.size(); ++i) {
        const auto &source = s.source.zones[i];
        auto top = remap(s.source.separateDisplayLists ? source.channelIndexes : members(source, false));
        auto bottom = s.source.separateDisplayLists ? remap(source.bottomChannelIndexes) : top;
        if (!target.separateDisplayLists && s.source.separateDisplayLists) top = remap(members(source, true));
        const bool emptied = !members(source, s.source.separateDisplayLists).isEmpty()
            && top.isEmpty() && (!target.separateDisplayLists || bottom.isEmpty());
        if (emptied) {
            zoneRows.append(choice(i, source.name, false, false, "No channels remain after conversion."));
            losses.append(QString("Repeater/zone ‘%1’: omitted because none of its channels will transfer.").arg(source.name));
            continue;
        }
        const int count = std::max(1, int((std::max(top.size(), target.separateDisplayLists ? bottom.size() : qsizetype(0)) + perZone - 1) / perZone));
        const bool keep = s.zones.contains(i);
        zoneRows.append(choice(i, source.name, keep, true, QString("Destination repeaters/zones: %1. This selection controls memberships; selected channels remain in the channel table.").arg(count)));
        if (!keep) { losses.append(QString("Repeater/zone ‘%1’ and its memberships: not selected.").arg(source.name)); continue; }
        if (count > 1) changes.append(QString("Repeater/zone ‘%1’ split into %2 repeaters/zones (%3 channels per list).").arg(source.name).arg(count).arg(perZone));
        if (s.source.separateDisplayLists != target.separateDisplayLists)
            changes.append(QString("Repeater/zone ‘%1’: %2.").arg(source.name, target.separateDisplayLists ? "channels placed on both Top and Bottom" : "Top and Bottom combined, retaining Top order followed by remaining Bottom channels"));
        const QString original = fitName(source.name, limits, "zone_name");
        for (int part = 0; part < count; ++part) {
            ImportedZone zone;
            const auto rule = nameRule(limits, "zone_name");
            const qsizetype nameMaximum = limitNumber(rule.value(rule.value("type") == "utf16le" ? "maximum_code_units" : "maximum_characters"));
            const QString suffix = " " + QString::number(part + 1);
            const QString label = count == 1 ? original : original.left(std::max(qsizetype(0), nameMaximum - suffix.size())).trimmed() + suffix;
            zone.name = fitName(label, limits, "zone_name", &zoneNames);
            if (count == 1 && zone.name != source.name) changes.append(QString("Repeater/zone ‘%1’ renamed to ‘%2’.").arg(source.name, zone.name));
            zone.channelIndexes = top.mid(part * perZone, perZone);
            if (target.separateDisplayLists) zone.bottomChannelIndexes = bottom.mid(part * perZone, perZone);
            if (source.repeaterTemplate && zone.channelIndexes.isEmpty() && zone.bottomChannelIndexes.isEmpty()
                && channelReason(*source.repeaterTemplate).isEmpty()) zone.repeaterTemplate = adaptChannel(*source.repeaterTemplate, false);
            target.zones.append(zone);
        }
    }
    const QString sourceName = s.source.radioName.trimmed();
    target.radioName = fitName(sourceName.isEmpty() ? s.baseline.radioName : sourceName, limits, "radio_name");
    if (target.radioName != s.source.radioName) changes.append(QString("Radio name set to ‘%1’.").arg(target.radioName));
    const auto idRange = limits.value("radio_id").toObject();
    target.radioId = s.source.radioId;
    if (target.radioId < int(limitNumber(idRange.value("minimum"))) || target.radioId > int(limitNumber(idRange.value("maximum")))) {
        target.radioId = std::max(int(limitNumber(idRange.value("minimum"))), s.baseline.radioId);
        changes.append(QString("Radio ID set to %1 because the source ID is blank or unsupported.").arg(target.radioId));
    }
    for (const auto &table : {QString("channels"), QString("contacts"), QString("zones")}) {
        const int count = table == "channels" ? target.channels.size() : table == "contacts" ? target.contacts.size() : target.zones.size();
        const auto bound = limits.value(table).toObject();
        if (count < limitNumber(bound.value("minimum")) || count > limitNumber(bound.value("maximum")))
            blockers.append(QString("%1: %2 selected; destination requires %3–%4. Adjust the selections.").arg(table == "zones" ? "Repeaters/Zones" : table).arg(count).arg(limitNumber(bound.value("minimum"))).arg(limitNumber(bound.value("maximum"))));
    }
    const QString notice = "Only settings represented in Waffle transfer. Other radio-specific settings, including scan-list and receive-group configuration, do not transfer. Contact usage includes channel references and other references declared by the source definition.";
    s.converted = std::move(target);
    s.review = {{"target", s.baseline.model}, {"contacts", contactRows}, {"channels", channelRows}, {"zones", zoneRows},
        {"changes", changes}, {"losses", losses}, {"blockers", blockers}, {"canConvert", blockers.isEmpty()}, {"notice", notice},
        {"contactCount", s.converted.contacts.size()}, {"contactCapacity", s.baseline.contactCapacity},
        {"channelCount", s.converted.channels.size()}, {"channelCapacity", s.baseline.channelCapacity},
        {"zoneCount", s.converted.zones.size()}, {"zoneCapacity", s.baseline.zoneCapacity},
        {"bandSummary", s.twoMeters && s.seventyCm ? "VHF and UHF" : s.twoMeters ? "VHF" : "UHF"}};
}
