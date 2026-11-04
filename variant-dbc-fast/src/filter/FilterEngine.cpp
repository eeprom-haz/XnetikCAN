#include "FilterEngine.h"

bool CanFilter::accepts(const QCanBusFrame &f) const {
    const quint32 id = f.frameId();
    if (hiddenIds.contains(id)) return false;
    if (!onlyIds.isEmpty() && !onlyIds.contains(id)) return false;
    const bool inRange = id >= idFrom && id <= idTo;
    if (inRange == excludeRange) return false;
    if (useMask && (id & mask) != (match & mask)) return false;
    if (!payloadContains.isEmpty() && !f.payload().contains(payloadContains)) return false;
    return true;
}

bool CanFilter::isPassAll() const {
    return idFrom == 0 && idTo == 0x1FFFFFFF && !excludeRange && !useMask &&
           payloadContains.isEmpty() && onlyIds.isEmpty() && hiddenIds.isEmpty();
}

QString CanFilter::describe() const {
    if (isPassAll()) return "Showing all messages";
    QStringList parts;
    if (!presetName.isEmpty()) parts << presetName;
    if (!onlyIds.isEmpty() && presetName.isEmpty())
        parts << QString("only %1 picked message%2").arg(onlyIds.size()).arg(onlyIds.size() == 1 ? "" : "s");
    if (!hiddenIds.isEmpty())
        parts << QString("%1 message%2 hidden").arg(hiddenIds.size()).arg(hiddenIds.size() == 1 ? "" : "s");
    if (presetName.isEmpty()) {
        if (idFrom != 0 || idTo != 0x1FFFFFFF)
            parts << QString("%1 IDs 0x%2–0x%3").arg(excludeRange ? "hiding" : "only")
                         .arg(idFrom, 0, 16).arg(idTo, 0, 16);
        if (useMask)
            parts << QString("ID mask 0x%1 = 0x%2").arg(mask, 0, 16).arg(match, 0, 16);
    }
    if (!payloadContains.isEmpty())
        parts << QString("data contains %1").arg(QString::fromLatin1(payloadContains.toHex(' ').toUpper()));
    return parts.join(" · ");
}
