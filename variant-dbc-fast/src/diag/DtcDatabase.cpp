#include "DtcDatabase.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

void DtcDatabase::load() {
    m_text.clear();
    const QDir dir(QCoreApplication::applicationDirPath() + "/library/dtc");
    for (const QString &name : dir.entryList({"*.csv"}, QDir::Files, QDir::Name)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            const int comma = line.indexOf(',');
            if (comma != 5) continue;  // "P0301,..." — skips headers and blank lines
            QString text = line.mid(comma + 1).trimmed();
            if (text.startsWith('"') && text.endsWith('"')) text = text.mid(1, text.size() - 2);
            m_text.insert(line.left(5).toUpper(), text.replace("\"\"", "\""));
        }
    }
}

QString DtcDatabase::describe(const QString &code) const {
    return m_text.value(code.toUpper());
}

bool DtcDatabase::manufacturerSpecific(const QString &code) {
    // Second character: 0 = SAE generic, 1 = manufacturer, 2 = generic (P) , 3 = mixed.
    if (code.size() < 2) return false;
    const QChar d = code[1];
    if (code[0] == 'P') return d == '1' || (d == '3' && code.mid(2, 1) < "4");
    return d == '1' || d == '2';
}

QString DtcDatabase::system(const QString &code) {
    if (code.size() != 5) return {};
    const QChar letter = code[0];
    if (letter == 'B') return "Body";
    if (letter == 'C') return "Chassis";
    if (letter == 'U') return "Network / communication";
    // Powertrain: third character gives the subsystem (J2012).
    switch (code[2].toLatin1()) {
    case '0': return "Fuel/air metering, auxiliary emission controls";
    case '1':
    case '2': return "Fuel and air metering";
    case '3': return "Ignition system or misfire";
    case '4': return "Auxiliary emission controls";
    case '5': return "Vehicle speed, idle control, auxiliary inputs";
    case '6': return "Computer and output circuits";
    case '7':
    case '8':
    case '9': return "Transmission";
    case 'A':
    case 'B':
    case 'C': return "Hybrid propulsion";
    default:  return "Powertrain";
    }
}
