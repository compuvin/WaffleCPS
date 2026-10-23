#pragma once

#include <QByteArray>
#include <QString>

// Encode the entire downloaded image, preserving all decoded bytes.
bool radioCodeplugFromRaw(const QByteArray &raw, QByteArray *encoded, QString *error);
