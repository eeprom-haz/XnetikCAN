#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QPalette>
#include <QStyleHints>
#include "MainWindow.h"

// The UI is designed light-only. Without this, Windows dark mode hands Qt a
// dark palette (white text) underneath our light stylesheet backgrounds.
static void forceLightTheme(QApplication &app) {
    app.styleHints()->setColorScheme(Qt::ColorScheme::Light);
    QApplication::setStyle("Fusion");  // honours the palette; windows11 style partly ignores it

    QPalette pal;
    const QColor text("#1a1a1a"), window("#f3f3f3"), base("#ffffff");
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, base);
    pal.setColor(QPalette::AlternateBase, QColor("#f9f9f9"));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, QColor("#fdfdfd"));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::ToolTipBase, base);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::PlaceholderText, QColor("#8a8a8a"));
    pal.setColor(QPalette::Highlight, QColor("#cce4ff"));
    pal.setColor(QPalette::HighlightedText, QColor("#000000"));
    pal.setColor(QPalette::Link, QColor("#0078d4"));
    pal.setColor(QPalette::Light, base);
    pal.setColor(QPalette::Midlight, QColor("#e8e8e8"));
    pal.setColor(QPalette::Mid, QColor("#c8c8c8"));
    pal.setColor(QPalette::Dark, QColor("#a0a0a0"));
    pal.setColor(QPalette::Shadow, QColor("#808080"));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        pal.setColor(QPalette::Disabled, role, QColor("#9a9a9a"));
    QApplication::setPalette(pal);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("XnetikCan");
    app.setOrganizationName("XnetikCan");
    app.setWindowIcon(QIcon(":/icons/logo.png"));

    forceLightTheme(app);

    QFile qss(":/style/fluent-light.qss");
    if (qss.open(QFile::ReadOnly))
        app.setStyleSheet(QLatin1String(qss.readAll()));

    MainWindow w;
    w.show();
    return app.exec();
}
