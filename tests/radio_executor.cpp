#include "../src/radioexecutor.h"
#include "../src/codeplugdefinition.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <stdexcept>
#include <string>

static void check(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    try {
        const QString base = QStringLiteral(WAFFLE_SOURCE_DIR "/definitions/cs7000-m17-plus");
        RadioDefinition radio;
        CodeplugDefinition codeplug;
        QString error;
        check(RadioDefinition::loadProfile(base + QStringLiteral(".wcp"), &radio, &codeplug, nullptr, &error),
              "Packaged radio definition failed to load");
        check(CodeplugDefinition::load(base + QStringLiteral(".wcp"), &codeplug, &error),
              "Packaged codeplug definition failed to load");
        QByteArray decoded;
        check(codeplug.decodedImage(codeplug.defaultCodeplug(), &decoded, &error),
              "Packaged default did not decode");
        const RadioExecutor::Bytes image(decoded.begin(), decoded.end());
        const auto blocks = RadioExecutor::planWrite(radio, codeplug.document(), image);
        check(!blocks.empty(), "Write plan selected no blocks");
        RadioExecutor::Bytes sparse(0x1401 + 0x400000, 0xA6);
        const auto put32 = [&](size_t at, uint32_t value) {
            for (unsigned byte = 0; byte < 4; ++byte) sparse.at(at + byte) = uint8_t(value >> (8 * byte));
        };
        const std::string model("CS7000-M17 PLUS\0", 16), version("V1.02.20.00\0", 12);
        std::copy(model.begin(), model.end(), sparse.begin() + 0x1200);
        std::copy(version.begin(), version.end(), sparse.begin() + 0x1298);
        put32(0x240, 0x1401);
        put32(0x1264, 0x1401);
        put32(0x1401, 335);
        put32(0x1401 + 4, 0x400000);
        for (uint32_t row = 0; row < 335; ++row) put32(0x1401 + 8 + row * 4, row + 1);
        const auto sparseNew = RadioExecutor::planWrite(radio, codeplug.document(), sparse);
        check(sparseNew.size() == 7, "Sparse write selected the wrong number of blocks");
        const auto &first = blocks.front();
        check(first.command == 0x11 && first.selector == 0 && first.payload.size() == 2056,
              "First write block differs from packaged definition");
        check(RadioExecutor::readSelector(radio, QStringLiteral("main"), 1, 5) == 0xC001,
              "Read selector differs from packaged definition");
        const RadioExecutor::Bytes request = RadioExecutor::makeFrame(radio, 0x05, 0x10, {0, 0, 8, 0});
        check(request == RadioExecutor::Bytes({0xA1, 0x05, 0x10, 0, 4, 0, 0, 8, 0, 0, 8, 0xA4}),
              "Request frame differs from packaged definition");

        QJsonObject changed = radio.document();
        QJsonObject methods = changed.value(QStringLiteral("methods")).toObject();
        QJsonObject write = methods.value(QStringLiteral("write_codeplug")).toObject();
        QJsonObject regions = write.value(QStringLiteral("regions")).toObject();
        QJsonObject main = regions.value(QStringLiteral("main")).toObject();
        main.insert(QStringLiteral("command"), QStringLiteral("0x31"));
        regions.insert(QStringLiteral("main"), main);
        write.insert(QStringLiteral("regions"), regions);
        methods.insert(QStringLiteral("write_codeplug"), write);
        changed.insert(QStringLiteral("methods"), methods);
        RadioDefinition changedRadio;
        check(RadioDefinition::fromJson(QJsonDocument(changed).toJson(), &changedRadio, &error),
              "Changed definition was rejected");
        const auto changedBlocks = RadioExecutor::planWrite(changedRadio, codeplug.document(), image);
        check(changedBlocks.front().command == 0x31,
              "Write plan ignored changed definition command");

        QJsonObject changedRead = radio.document();
        methods = changedRead.value(QStringLiteral("methods")).toObject();
        QJsonObject readBlock = methods.value(QStringLiteral("read_block")).toObject();
        QJsonObject selector = readBlock.value(QStringLiteral("selector")).toObject();
        QJsonArray rules = selector.value(QStringLiteral("rules_in_priority_order")).toArray();
        QJsonObject fallback = rules.at(2).toObject();
        fallback.insert(QStringLiteral("or_with_block_index"), QStringLiteral("0x8000"));
        rules.replace(2, fallback);
        selector.insert(QStringLiteral("rules_in_priority_order"), rules);
        readBlock.insert(QStringLiteral("selector"), selector);
        methods.insert(QStringLiteral("read_block"), readBlock);
        changedRead.insert(QStringLiteral("methods"), methods);
        RadioDefinition changedReader;
        check(RadioDefinition::fromJson(QJsonDocument(changedRead).toJson(), &changedReader, &error),
              "Changed read selector was rejected");
        check(RadioExecutor::readSelector(changedReader, QStringLiteral("main"), 1, 5) == 0x8001,
              "Read selector ignored changed definition rule");

        QJsonObject changedFrame = radio.document();
        QJsonObject frame = changedFrame.value(QStringLiteral("frame")).toObject();
        QJsonArray frameFields = frame.value(QStringLiteral("fields")).toArray();
        QJsonObject delimiter = frameFields.at(0).toObject();
        delimiter.insert(QStringLiteral("value"), QStringLiteral("0xA2"));
        frameFields.replace(0, delimiter);
        frame.insert(QStringLiteral("fields"), frameFields);
        changedFrame.insert(QStringLiteral("frame"), frame);
        RadioDefinition changedFraming;
        check(RadioDefinition::fromJson(QJsonDocument(changedFrame).toJson(), &changedFraming, &error),
              "Changed frame delimiter was rejected");
        check(RadioExecutor::makeFrame(changedFraming, 0x05, 0x10, {0, 0, 8, 0}).front() == 0xA2,
              "Request frame ignored changed definition delimiter");
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
