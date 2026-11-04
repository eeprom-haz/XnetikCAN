#include "DbcFile.h"
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>

bool DbcSignal::decode(const QByteArray &payload, double *physical, qint64 *rawOut) const {
    const auto *d = reinterpret_cast<const quint8 *>(payload.constData());
    const int nbits = payload.size() * 8;
    auto bitAt = [&](int pos) -> quint64 { return (d[pos / 8] >> (pos % 8)) & 1u; };

    quint64 raw = 0;
    if (littleEndian) {
        // Intel: startBit is the LSB; bits run upward through the bytes.
        if (startBit + length > nbits) return false;
        for (int i = 0; i < length; ++i)
            raw |= bitAt(startBit + i) << i;
    } else {
        // Motorola (DBC "sawtooth" numbering): startBit is the MSB.
        int pos = startBit;
        for (int i = 0; i < length; ++i) {
            if (pos < 0 || pos >= nbits) return false;
            raw = (raw << 1) | bitAt(pos);
            pos = (pos % 8 == 0) ? pos + 15 : pos - 1;
        }
    }

    qint64 value = qint64(raw);
    if (isSigned && length < 64 && (raw >> (length - 1)) & 1u)
        value -= qint64(1) << length;
    if (rawOut) *rawOut = value;
    *physical = double(value) * factor + offset;
    return true;
}

QString DbcSignal::format(double physical, qint64 raw) const {
    auto it = values.constFind(raw);
    if (it != values.constEnd()) return it.value();
    if (factor == std::floor(factor) && offset == std::floor(offset))
        return QString::number(qint64(physical));
    return QString::number(physical, 'f', std::fabs(physical) >= 100 ? 1 : 2);
}

const DbcMessage *DbcFile::message(quint32 id) const {
    auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : &m_messages[it.value()];
}

bool DbcFile::write(const QString &path, const QList<DbcMessage> &messages, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    QTextStream out(&f);
    auto num = [](double v) { return QString::number(v, 'g', 10); };   // round-trip-safe
    out << "VERSION \"\"\n\n\nBS_:\n\nBU_: Tester\n\n";
    QStringList valStatements;
    for (const DbcMessage &m : messages) {
        const quint32 rawId = m.id | (m.extended ? 0x80000000u : 0u);
        out << QString("BO_ %1 %2: %3 Tester\n").arg(rawId).arg(m.name).arg(m.dlc);
        for (const DbcSignal &s : m.signalList) {
            out << QString(" SG_ %1 : %2|%3@%4%5 (%6,%7) [%8|%9] \"%10\" Tester\n")
                       .arg(s.name).arg(s.startBit).arg(s.length)
                       .arg(s.littleEndian ? 1 : 0).arg(s.isSigned ? '-' : '+')
                       .arg(num(s.factor), num(s.offset), num(s.min), num(s.max), s.unit);
        }
        out << "\n";
        // Enumerations (VAL_) and comments (CM_) for this message's signals.
        for (const DbcSignal &s : m.signalList) {
            if (!s.values.isEmpty()) {
                QString v = QString("VAL_ %1 %2").arg(rawId).arg(s.name);
                for (auto it = s.values.cbegin(); it != s.values.cend(); ++it)
                    v += QString(" %1 \"%2\"").arg(it.key()).arg(it.value());
                valStatements << v + " ;";
            }
            if (!s.comment.isEmpty())
                valStatements << QString("CM_ SG_ %1 %2 \"%3\" ;").arg(rawId).arg(s.name, s.comment);
        }
    }
    for (const QString &v : valStatements) out << v << "\n";
    return true;
}

// Splits "a b \"quoted text\" c" into tokens, keeping quoted text as one token.
static QStringList tokenize(const QString &s) {
    QStringList out;
    QString cur;
    bool inQuote = false;
    for (QChar c : s) {
        if (c == '"') {
            if (inQuote) { out << cur; cur.clear(); }
            inQuote = !inQuote;
        } else if (!inQuote && c.isSpace()) {
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

bool DbcFile::load(const QString &path, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    m_messages.clear();
    m_index.clear();
    m_path = path;

    static const QRegularExpression reBo(R"re(^BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\w+))re");
    static const QRegularExpression reSg(
        R"re(^SG_\s+(\w+)\s*(\w*)\s*:\s*(\d+)\|(\d+)@([01])([+-])\s*\(\s*([^,]+)\s*,\s*([^)]+)\)\s*)re"
        R"re(\[\s*([^|]*)\|([^\]]*)\]\s*"([^"]*)")re");

    // Read whole file; statements like CM_ and VAL_ may span lines until ';'.
    QTextStream in(&f);
    const QStringList lines = in.readAll().split('\n');
    DbcMessage *current = nullptr;
    QList<DbcMessage> msgs;
    QHash<quint32, int> index;
    QString pending;  // multi-line CM_/VAL_ statement being collected

    auto handleStatement = [&](const QString &stmt) {
        const QStringList t = tokenize(stmt.trimmed().chopped(stmt.trimmed().endsWith(';') ? 1 : 0));
        if (t.size() < 2) return;
        if (t[0] == "CM_") {
            if (t.size() >= 4 && t[1] == "BO_") {
                auto it = index.find(t[2].toUInt() & 0x1FFFFFFF);
                if (it != index.end()) msgs[*it].comment = t[3];
            } else if (t.size() >= 5 && t[1] == "SG_") {
                auto it = index.find(t[2].toUInt() & 0x1FFFFFFF);
                if (it == index.end()) return;
                for (DbcSignal &s : msgs[*it].signalList)
                    if (s.name == t[3]) s.comment = t[4];
            }
        } else if (t[0] == "VAL_" && t.size() >= 4) {
            auto it = index.find(t[1].toUInt() & 0x1FFFFFFF);
            if (it == index.end()) return;
            for (DbcSignal &s : msgs[*it].signalList) {
                if (s.name != t[2]) continue;
                for (int i = 3; i + 1 < t.size(); i += 2)
                    s.values.insert(t[i].toLongLong(), t[i + 1]);
            }
        }
    };

    for (QString line : lines) {
        line = line.trimmed();
        if (!pending.isEmpty()) {
            pending += ' ' + line;
            if (pending.count('"') % 2 == 0 && pending.endsWith(';')) {
                handleStatement(pending);
                pending.clear();
            }
            continue;
        }
        if (line.startsWith("BO_ ")) {
            auto m = reBo.match(line);
            current = nullptr;
            if (!m.hasMatch()) continue;
            DbcMessage msg;
            const quint32 rawId = m.captured(1).toUInt();
            msg.extended = rawId & 0x80000000u;
            msg.id = rawId & 0x1FFFFFFF;
            msg.name = m.captured(2);
            msg.dlc = m.captured(3).toInt();
            msg.sender = m.captured(4);
            index.insert(msg.id, msgs.size());
            msgs.append(msg);
            current = &msgs.last();
        } else if (line.startsWith("SG_ ") && current) {
            auto m = reSg.match(line);
            if (!m.hasMatch() || !m.captured(2).isEmpty()) continue;  // skip multiplexed
            DbcSignal s;
            s.name = m.captured(1);
            s.startBit = m.captured(3).toInt();
            s.length = m.captured(4).toInt();
            s.littleEndian = m.captured(5) == "1";
            s.isSigned = m.captured(6) == "-";
            s.factor = m.captured(7).trimmed().toDouble();
            s.offset = m.captured(8).trimmed().toDouble();
            s.min = m.captured(9).trimmed().toDouble();
            s.max = m.captured(10).trimmed().toDouble();
            s.unit = m.captured(11);
            if (s.length > 0 && s.length <= 64) current->signalList.append(s);
        } else if (line.startsWith("CM_ ") || line.startsWith("VAL_ ")) {
            current = nullptr;
            if (line.count('"') % 2 == 0 && line.endsWith(';')) handleStatement(line);
            else pending = line;
        } else if (!line.isEmpty()) {
            current = nullptr;
        }
    }

    m_messages = msgs;
    for (int i = 0; i < m_messages.size(); ++i) m_index.insert(m_messages[i].id, i);
    if (m_messages.isEmpty() && error) *error = "No messages found in " + path;
    return !m_messages.isEmpty();
}
