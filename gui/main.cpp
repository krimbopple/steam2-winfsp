#include "main_window.hpp"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("steam2-winfsp"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("github.com/dr3murr/steam2-winfsp"));
    QCoreApplication::setApplicationName(QStringLiteral("Steam2 Depot Manager"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.1"));

    qRegisterMetaType<s2gui::Catalog>();

    s2gui::MainWindow window;
    window.show();
    return application.exec();
}
