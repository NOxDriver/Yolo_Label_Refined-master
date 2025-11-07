#include "mainwindow.h"
#include <QApplication>
#include <QCoreApplication>


int main(int argc, char *argv[]) {
    QCoreApplication::setOrganizationName("WildlifeSpotter");
    QCoreApplication::setApplicationName("YoloLabel");
    QApplication a(argc, argv);
    MainWindow w;
    w.show();
    return a.exec();
}