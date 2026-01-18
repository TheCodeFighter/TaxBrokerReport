#include <QApplication>
#include "main_window.hpp"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    app.setDesktopFileName("EdavkiXmlMaker");

    MainWindow w;
    w.show();
    return app.exec();
}