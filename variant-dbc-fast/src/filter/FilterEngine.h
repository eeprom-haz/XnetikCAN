#pragma once
#include <QCanBusFrame>
#include <QString>
#include <QStringList>
#include <QSet>

struct CanFilter {
    // Advanced (raw) criteria
    quint32 idFrom = 0;
    quint32 idTo = 0x1FFFFFFF;
    bool excludeRange = false;      // hide IDs in the range instead of keeping them
    quint32 mask = 0;
    quint32 match = 0;
    bool useMask = false;
    QByteArray payloadContains;

    // Point-and-click criteria
    QSet<quint32> onlyIds;          // non-empty: show only these messages
    QSet<quint32> hiddenIds;        // never show these

    QString presetName;             // shown in the plain-language summary

    bool accepts(const QCanBusFrame &f) const;
    bool isPassAll() const;
    QString describe() const;       // plain language, also used in the AI context
};
