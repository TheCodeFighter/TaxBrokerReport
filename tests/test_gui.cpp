#include <gtest/gtest.h>
#include <QApplication>
#include "main_window.hpp"

int argc = 0;
char **argv = nullptr;

TEST(GuiTest, SmokeTest) {
    // App need to exist
    if (!QApplication::instance()) {
        new QApplication(argc, argv);
    }

    MainWindow w;
    // Window need to be generated without crash
    EXPECT_TRUE(w.isEnabled());
}