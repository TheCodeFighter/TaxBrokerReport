#include <gtest/gtest.h>
#include <QApplication>
#include "main_window.hpp"

TEST(GuiTest, SmokeTest) {
    MainWindow w;
    w.show();
    QApplication::processEvents();
    EXPECT_TRUE(w.isVisible());
    w.close();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    int result = RUN_ALL_TESTS();
    
    QApplication::processEvents();
    return result;
}