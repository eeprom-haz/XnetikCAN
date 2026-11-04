#pragma once
#include <QString>

// Plain-text logs in <AppData>/logs/<name>-<date>.log, one line per event with the time.
// "requests": what the app asked each module and what came back (checks in full, polling as
// first answers, refusals, missed replies and a summary every 10 s), sessions, vehicle ID.
namespace DiagLog {
QString path(const QString &name);
void write(const QString &name, const QString &line);
}
