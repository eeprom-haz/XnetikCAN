#pragma once
#include <QHash>
#include <QString>

// Code descriptions from <exe>/library/dtc/*.csv ("P0301,Cylinder 1 Misfire Detected").
// Codes not in any list still get their system from the SAE J2012 code structure.
class DtcDatabase {
public:
    void load();
    int size() const { return m_text.size(); }
    QString describe(const QString &code) const;   // listed text, or "" if unknown
    static QString system(const QString &code);    // e.g. "Ignition system / misfire"
    static bool manufacturerSpecific(const QString &code);

private:
    QHash<QString, QString> m_text;
};
