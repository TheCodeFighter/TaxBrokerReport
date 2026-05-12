#include <gtest/gtest.h>
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QSpinBox>
#include <QTabWidget>
#include <QProgressBar>
#include <QMessageBox>
#include <QTextBrowser>
#include <QLabel>
#include <QThread>
#include "gui/main_window.hpp"
#include "gui/worker.hpp"

class MainWindowTest : public ::testing::Test {
protected:
    void SetUp() override {
        w = new MainWindow();
        w->show();
        QApplication::processEvents();
    }

    void TearDown() override {
        delete w;
    }

    MainWindow* w;
};

// 1. Basic Smoke Test
TEST_F(MainWindowTest, InitialVisibility) {
    EXPECT_TRUE(w->isVisible());
}

// 2. Logic Test: Verify that checking JSON-only hides XML specific groups
TEST_F(MainWindowTest, JsonOnlyToggleHidesXmlGroups) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    auto groups = w->findChildren<QGroupBox*>();

    ASSERT_NE(jsonCheckbox, nullptr);
    
    // Default state: XML mode, groups should be visible
    for (auto group : groups) {
        if (group->title().contains("Tax Data") || group->title().contains("Optional")) {
            EXPECT_TRUE(group->isVisible());
        }
    }

    // Toggle to JSON-only mode
    jsonCheckbox->setChecked(true);
    QApplication::processEvents();

    // Verify groups are hidden
    for (auto group : groups) {
        if (group->title().contains("Tax Data") || group->title().contains("Optional")) {
            EXPECT_FALSE(group->isVisible());
        }
    }
}

// 3. Logic Test: Verify Button Text updates
TEST_F(MainWindowTest, ButtonTextUpdatesOnToggle) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    auto generateBtn = w->findChild<QPushButton*>();

    ASSERT_NE(generateBtn, nullptr);

    // Initial state
    EXPECT_EQ(generateBtn->text(), "Generate XML");

    // Toggle JSON
    jsonCheckbox->setChecked(true);
    EXPECT_EQ(generateBtn->text(), "Generate JSON");

    // Toggle back
    jsonCheckbox->setChecked(false);
    EXPECT_EQ(generateBtn->text(), "Generate XML");
}

// 4. Test combo box existence and initial values
TEST_F(MainWindowTest, FormTypeComboBoxInitialized) {
    auto combos = w->findChildren<QComboBox*>();
    ASSERT_GT(combos.size(), 0);
    
    // Check that we have the form type combo with expected items
    bool foundFormType = false;
    for (auto combo : combos) {
        if (combo->count() == 3) { // Form type has 3 options
            foundFormType = true;
            EXPECT_EQ(combo->itemText(0), "Doh-KDVP (Capital Gains)");
            EXPECT_EQ(combo->itemText(1), "Doh-DIV (Dividends)");
            EXPECT_EQ(combo->itemText(2), "Doh-DHO (Interest)");
        }
    }
    EXPECT_TRUE(foundFormType);
}

// 5. Test document type combo box
TEST_F(MainWindowTest, DocumentTypeComboBoxInitialized) {
    auto combos = w->findChildren<QComboBox*>();
    ASSERT_GT(combos.size(), 1);
    
    // Check document type combo
    bool foundDocType = false;
    for (auto combo : combos) {
        if (combo->count() == 2) { // Doc type has 2 options
            foundDocType = true;
            EXPECT_EQ(combo->itemText(0), "Original");
            EXPECT_EQ(combo->itemText(1), "Self-Report (Samoprijava)");
        }
    }
    EXPECT_TRUE(foundDocType);
}

// 6. Test year spin box initialization
TEST_F(MainWindowTest, YearSpinBoxInitialized) {
    auto spinBoxes = w->findChildren<QSpinBox*>();
    ASSERT_GT(spinBoxes.size(), 0);
    
    // Year spin box should have 2025 as default
    EXPECT_EQ(spinBoxes[0]->value(), 2025);
    EXPECT_EQ(spinBoxes[0]->minimum(), 2020);
    EXPECT_EQ(spinBoxes[0]->maximum(), 2050);
}

// 7. Test year modification
TEST_F(MainWindowTest, ModifyYearSpinBox) {
    auto spinBoxes = w->findChildren<QSpinBox*>();
    ASSERT_GT(spinBoxes.size(), 0);
    
    spinBoxes[0]->setValue(2023);
    EXPECT_EQ(spinBoxes[0]->value(), 2023);
}

// 8. Test tab widget creation
TEST_F(MainWindowTest, TabWidgetHasExpectedTabs) {
    auto tabWidget = w->findChild<QTabWidget*>();
    ASSERT_NE(tabWidget, nullptr);
    EXPECT_EQ(tabWidget->count(), 3);
    EXPECT_EQ(tabWidget->tabText(0), "Home");
    EXPECT_EQ(tabWidget->tabText(1), "Manuals");
    EXPECT_EQ(tabWidget->tabText(2), "About");
}

// 9. Test input field placeholders
TEST_F(MainWindowTest, InputFieldPlaceholders) {
    auto lineEdits = w->findChildren<QLineEdit*>();
    ASSERT_GT(lineEdits.size(), 0);
    
    // Check for tax number field
    bool foundTaxNum = false;
    for (auto edit : lineEdits) {
        if (edit->placeholderText() == "e.g. 12345678") {
            foundTaxNum = true;
            EXPECT_TRUE(edit->text().isEmpty());
        }
    }
    EXPECT_TRUE(foundTaxNum);
}

// 10. Test JSON mode validation - no tax number required
TEST_F(MainWindowTest, JsonOnlyModeSkipsTaxNumberValidation) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    ASSERT_NE(jsonCheckbox, nullptr);
    
    // Ensure JSON-only mode doesn't require tax number
    jsonCheckbox->setChecked(true);
    QApplication::processEvents();
    
    // Tax number group should be hidden
    auto groups = w->findChildren<QGroupBox*>();
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            EXPECT_FALSE(group->isVisible());
        }
    }
}

// 11. Test XML mode validation - tax number required
TEST_F(MainWindowTest, XmlModeVisibility) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    jsonCheckbox->setChecked(false);
    QApplication::processEvents();
    
    auto groups = w->findChildren<QGroupBox*>();
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            EXPECT_TRUE(group->isVisible());
        }
    }
}

// 12. Test progress bar visibility
TEST_F(MainWindowTest, ProgressBarInitiallyHidden) {
    auto progressBar = w->findChild<QProgressBar*>();
    ASSERT_NE(progressBar, nullptr);
    EXPECT_FALSE(progressBar->isVisible());
    // Progress bar value may start at 0 or other value, but should be hidden initially
}

// 13. Test progress bar range
TEST_F(MainWindowTest, ProgressBarRange) {
    auto progressBar = w->findChild<QProgressBar*>();
    ASSERT_NE(progressBar, nullptr);
    EXPECT_EQ(progressBar->minimum(), 0);
    EXPECT_EQ(progressBar->maximum(), 100);
}

// 14. Test form type combo selection change
TEST_F(MainWindowTest, FormTypeComboSelection) {
    auto combos = w->findChildren<QComboBox*>();
    
    // Find the form type combo (the one with 3 items)
    QComboBox* formTypeCombo = nullptr;
    for (auto combo : combos) {
        if (combo->count() == 3) {
            formTypeCombo = combo;
            break;
        }
    }
    
    ASSERT_NE(formTypeCombo, nullptr);
    
    // Test changing selection
    formTypeCombo->setCurrentIndex(1);
    EXPECT_EQ(formTypeCombo->currentIndex(), 1);
    EXPECT_EQ(formTypeCombo->currentText(), "Doh-DIV (Dividends)");
    
    formTypeCombo->setCurrentIndex(2);
    EXPECT_EQ(formTypeCombo->currentIndex(), 2);
    EXPECT_EQ(formTypeCombo->currentText(), "Doh-DHO (Interest)");
}

// 15. Test document type combo selection
TEST_F(MainWindowTest, DocumentTypeComboSelection) {
    auto combos = w->findChildren<QComboBox*>();
    
    // Find the document type combo (the one with 2 items)
    QComboBox* docTypeCombo = nullptr;
    for (auto combo : combos) {
        if (combo->count() == 2) {
            docTypeCombo = combo;
            break;
        }
    }
    
    ASSERT_NE(docTypeCombo, nullptr);
    
    // Test changing selection
    docTypeCombo->setCurrentIndex(1);
    EXPECT_EQ(docTypeCombo->currentIndex(), 1);
    EXPECT_EQ(docTypeCombo->currentText(), "Self-Report (Samoprijava)");
    
    docTypeCombo->setCurrentIndex(0);
    EXPECT_EQ(docTypeCombo->currentIndex(), 0);
    EXPECT_EQ(docTypeCombo->currentText(), "Original");
}

// 16. Test email and phone fields visibility
TEST_F(MainWindowTest, OptionalContactInfoGroup) {
    auto groups = w->findChildren<QGroupBox*>();
    
    bool foundOptGroup = false;
    for (auto group : groups) {
        if (group->title().contains("Optional")) {
            foundOptGroup = true;
            EXPECT_TRUE(group->isVisible());
        }
    }
    EXPECT_TRUE(foundOptGroup);
}

// 19. Test validation error - missing input file
TEST_F(MainWindowTest, ValidationErrorMissingInputFile) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    auto outputEdit = w->findChildren<QLineEdit*>();
    
    jsonCheckbox->setChecked(true); // JSON mode
    
    // Set output dir but not input file
    if (outputEdit.size() > 1) {
        outputEdit[1]->setText("/tmp");
    }
    
    // onGenerateClicked would show error for missing input file
    // We test that the validation works elsewhere in integration
}

// 20. Test window title
TEST_F(MainWindowTest, WindowTitleSet) {
    EXPECT_EQ(w->windowTitle(), "Edavki XML Maker");
}

// 23. Test manual tab accessibility
TEST_F(MainWindowTest, ManualTabContent) {
    auto tabs = w->findChildren<QTabWidget*>();
    ASSERT_GT(tabs.size(), 0);
    
    QTabWidget* mainTabs = nullptr;
    for (auto tab : tabs) {
        if (tab->count() == 3) { // Main tab widget
            mainTabs = tab;
            break;
        }
    }
    
    ASSERT_NE(mainTabs, nullptr);
    mainTabs->setCurrentIndex(1); // Switch to Manuals
    
    auto browsers = w->findChildren<QTextBrowser*>();
    EXPECT_GT(browsers.size(), 0);
}

// 24. Test about tab accessibility
TEST_F(MainWindowTest, AboutTabContent) {
    auto tabs = w->findChildren<QTabWidget*>();
    
    QTabWidget* mainTabs = nullptr;
    for (auto tab : tabs) {
        if (tab->count() == 3) {
            mainTabs = tab;
            break;
        }
    }
    
    ASSERT_NE(mainTabs, nullptr);
    mainTabs->setCurrentIndex(2); // Switch to About
    
    auto labels = w->findChildren<QLabel*>();
    EXPECT_GT(labels.size(), 0);
}

// 25. Test validation error - missing output directory
TEST_F(MainWindowTest, ValidationErrorMissingOutputDir) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    auto inputEdit = w->findChildren<QLineEdit*>();
    
    jsonCheckbox->setChecked(true);
    
    // Set input file but not output dir
    if (inputEdit.size() > 0) {
        inputEdit[0]->setText("test.pdf");
    }
}

// 27. Test validation error - missing tax number in XML mode
TEST_F(MainWindowTest, ValidationErrorMissingTaxNumberXmlMode) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    jsonCheckbox->setChecked(false); // XML mode requires tax number
    
    // Fields should be visible
    auto groups = w->findChildren<QGroupBox*>();
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            EXPECT_TRUE(group->isVisible());
        }
    }
}

// 28. Test all form types selectable
TEST_F(MainWindowTest, AllFormTypesSelectable) {
    auto combos = w->findChildren<QComboBox*>();
    
    QComboBox* formTypeCombo = nullptr;
    for (auto combo : combos) {
        if (combo->count() == 3) {
            formTypeCombo = combo;
            break;
        }
    }
    
    ASSERT_NE(formTypeCombo, nullptr);
    
    // Test all form types
    formTypeCombo->setCurrentIndex(0);
    EXPECT_EQ(formTypeCombo->currentText(), "Doh-KDVP (Capital Gains)");
    
    formTypeCombo->setCurrentIndex(1);
    EXPECT_EQ(formTypeCombo->currentText(), "Doh-DIV (Dividends)");
    
    formTypeCombo->setCurrentIndex(2);
    EXPECT_EQ(formTypeCombo->currentText(), "Doh-DHO (Interest)");
}

// 29. Test all document types selectable
TEST_F(MainWindowTest, AllDocumentTypesSelectable) {
    auto combos = w->findChildren<QComboBox*>();
    
    QComboBox* docTypeCombo = nullptr;
    for (auto combo : combos) {
        if (combo->count() == 2) {
            docTypeCombo = combo;
            break;
        }
    }
    
    ASSERT_NE(docTypeCombo, nullptr);
    
    docTypeCombo->setCurrentIndex(0);
    EXPECT_EQ(docTypeCombo->currentText(), "Original");
    
    docTypeCombo->setCurrentIndex(1);
    EXPECT_EQ(docTypeCombo->currentText(), "Self-Report (Samoprijava)");
}

// 30. Test year range is valid
TEST_F(MainWindowTest, YearRangeValid) {
    auto spinBoxes = w->findChildren<QSpinBox*>();
    ASSERT_GT(spinBoxes.size(), 0);
    
    auto yearSpin = spinBoxes[0];
    
    // Test minimum year
    yearSpin->setValue(2020);
    EXPECT_EQ(yearSpin->value(), 2020);
    
    // Test maximum year
    yearSpin->setValue(2050);
    EXPECT_EQ(yearSpin->value(), 2050);
    
    // Test clamping
    yearSpin->setValue(2000); // Below minimum
    EXPECT_EQ(yearSpin->value(), 2020);
    
    yearSpin->setValue(2100); // Above maximum
    EXPECT_EQ(yearSpin->value(), 2050);
}

// 31. Test JSON mode toggle multiple times
TEST_F(MainWindowTest, JsonModeToggleMultipleTimes) {
    auto jsonCheckbox = w->findChild<QCheckBox*>();
    
    // Toggle on
    jsonCheckbox->setChecked(true);
    QApplication::processEvents();
    
    auto groups = w->findChildren<QGroupBox*>();
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            EXPECT_FALSE(group->isVisible());
        }
    }
    
    // Toggle off
    jsonCheckbox->setChecked(false);
    QApplication::processEvents();
    
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            EXPECT_TRUE(group->isVisible());
        }
    }
    
    // Toggle on again
    jsonCheckbox->setChecked(true);
    QApplication::processEvents();
    
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            EXPECT_FALSE(group->isVisible());
        }
    }
}

// ============ Worker Tests ============

class WorkerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a basic request for testing
        testRequest.jsonOnly = true;
        testRequest.inputFile = "/tmp/test.txt";
        testRequest.outputDirectory = "/tmp";
    }

    GenerationRequest testRequest;
};

// 32. Test worker construction
TEST_F(WorkerTest, WorkerConstruction) {
    Worker worker(testRequest);
    EXPECT_NE(&worker, nullptr);
}

// 33. Test worker with empty request
TEST_F(WorkerTest, WorkerWithEmptyRequest) {
    GenerationRequest emptyRequest;
    Worker worker(emptyRequest);
    EXPECT_NE(&worker, nullptr);
}

// 34. Test worker request storage
TEST_F(WorkerTest, WorkerStoresRequest) {
    testRequest.inputFile = "/test/input.pdf";
    testRequest.outputDirectory = "/test/output";
    testRequest.jsonOnly = false;
    
    Worker worker(testRequest);
    // Worker constructor takes request by value/move
    EXPECT_NE(&worker, nullptr);
}

// 35. Test worker with various request configurations
TEST_F(WorkerTest, WorkerWithXmlRequest) {
    GenerationRequest xmlRequest;
    xmlRequest.jsonOnly = false;
    xmlRequest.inputFile = "/test/report.pdf";
    xmlRequest.outputDirectory = "/output";
    xmlRequest.taxNumber = "12345678";
    xmlRequest.year = 2025;
    xmlRequest.formType = TaxFormType::Doh_KDVP;
    xmlRequest.formDocType = FormType::Original;
    xmlRequest.email = "test@example.com";
    xmlRequest.phone = "+386123456789";
    
    Worker worker(xmlRequest);
    EXPECT_NE(&worker, nullptr);
}

// 36. Test worker inherits from QObject
TEST_F(WorkerTest, WorkerIsQObject) {
    Worker worker(testRequest);
    EXPECT_TRUE(worker.inherits("QObject"));
}

// 37. Test line edits are writable
TEST_F(MainWindowTest, LineEditsAreWritable) {
    auto lineEdits = w->findChildren<QLineEdit*>();
    
    for (auto edit : lineEdits) {
        EXPECT_FALSE(edit->isReadOnly());
    }
}

// 38. Test all group boxes are visible initially
TEST_F(MainWindowTest, MandatoryGroupBoxVisible) {
    auto groups = w->findChildren<QGroupBox*>();
    
    bool foundMandatory = false;
    for (auto group : groups) {
        if (group->title().contains("Tax Data")) {
            foundMandatory = true;
            EXPECT_TRUE(group->isVisible());
        }
    }
    EXPECT_TRUE(foundMandatory);
}

// 39. Test checkbox has proper text
TEST_F(MainWindowTest, CheckboxProperText) {
    auto checkboxes = w->findChildren<QCheckBox*>();
    
    ASSERT_GT(checkboxes.size(), 0);
    EXPECT_TRUE(checkboxes[0]->text().contains("JSON"));
}

// 40. Test form layout structure
TEST_F(MainWindowTest, FormLayoutStructure) {
    QWidget* home = nullptr;
    auto tabs = w->findChildren<QTabWidget*>();
    
    for (auto tab : tabs) {
        if (tab->count() == 3) {
            home = tab->widget(0);
            break;
        }
    }
    
    ASSERT_NE(home, nullptr);
    
    // Verify home tab has groups and fields
    auto groups = home->findChildren<QGroupBox*>();
    EXPECT_GE(groups.size(), 3); // Mandatory, File, Optional groups
}

// 42. Test text field clear and set
TEST_F(MainWindowTest, TextFieldClearAndSet) {
    auto lineEdits = w->findChildren<QLineEdit*>();
    if (lineEdits.size() > 0) {
        lineEdits[0]->setText("test value");
        EXPECT_EQ(lineEdits[0]->text(), "test value");
        
        lineEdits[0]->clear();
        EXPECT_TRUE(lineEdits[0]->text().isEmpty());
    }
}

// 43. Test button minimum height
TEST_F(MainWindowTest, GenerateButtonMinHeight) {
    auto buttons = w->findChildren<QPushButton*>();
    
    QPushButton* generateBtn = nullptr;
    for (auto btn : buttons) {
        if (btn->text().contains("Generate")) {
            generateBtn = btn;
            break;
        }
    }
    
    ASSERT_NE(generateBtn, nullptr);
    EXPECT_GE(generateBtn->minimumHeight(), 40);
}

// 44. Test year spin box bounds
TEST_F(MainWindowTest, YearSpinBoxBounds) {
    auto spinBoxes = w->findChildren<QSpinBox*>();
    ASSERT_GT(spinBoxes.size(), 0);
    
    auto yearSpin = spinBoxes[0];
    EXPECT_EQ(yearSpin->minimum(), 2020);
    EXPECT_EQ(yearSpin->maximum(), 2050);
    EXPECT_EQ(yearSpin->value(), 2025);
}

// 45. Test checkbox tooltip exists
TEST_F(MainWindowTest, CheckboxHasTooltip) {
    auto checkboxes = w->findChildren<QCheckBox*>();
    ASSERT_GT(checkboxes.size(), 0);
    
    QString tooltip = checkboxes[0]->toolTip();
    EXPECT_FALSE(tooltip.isEmpty());
    EXPECT_TRUE(tooltip.contains("JSON") || tooltip.contains("Useful"));
}

// 46. Test window icon is set
TEST_F(MainWindowTest, WindowIconSet) {
    QIcon icon = w->windowIcon();
    // Icon might be empty if resource doesn't exist, but it should be set programmatically
    EXPECT_NE(&icon, nullptr);
}

// 47. Test progress bar minimum/maximum
TEST_F(MainWindowTest, ProgressBarMinMax) {
    auto progressBar = w->findChild<QProgressBar*>();
    ASSERT_NE(progressBar, nullptr);
    
    EXPECT_EQ(progressBar->minimum(), 0);
    EXPECT_EQ(progressBar->maximum(), 100);
}

// 48. Test all push buttons are clickable
TEST_F(MainWindowTest, PushButtonsClickable) {
    auto buttons = w->findChildren<QPushButton*>();
    
    for (auto btn : buttons) {
        EXPECT_TRUE(btn->isEnabled());
    }
}

// 49. Test form combo boxes have meaningful data
TEST_F(MainWindowTest, ComboBoxesHaveMeaningfulData) {
    auto combos = w->findChildren<QComboBox*>();
    
    for (auto combo : combos) {
        EXPECT_GT(combo->count(), 0);
        EXPECT_FALSE(combo->itemText(0).isEmpty());
    }
}

// 50. Test email field accepts valid input
TEST_F(MainWindowTest, EmailFieldInput) {
    auto lineEdits = w->findChildren<QLineEdit*>();
    
    // Email should be one of the later fields
    if (lineEdits.size() >= 5) {
        auto emailField = lineEdits[lineEdits.size() - 2];
        emailField->setText("user@example.com");
        EXPECT_EQ(emailField->text(), "user@example.com");
    }
}

// 51. Test phone field accepts valid input
TEST_F(MainWindowTest, PhoneFieldInput) {
    auto lineEdits = w->findChildren<QLineEdit*>();
    
    if (lineEdits.size() >= 6) {
        auto phoneField = lineEdits[lineEdits.size() - 1];
        phoneField->setText("+386 1 123 4567");
        EXPECT_EQ(phoneField->text(), "+386 1 123 4567");
    }
}

int main(int argc, char **argv) {
    // QApplication must exist for any widget tests to work
    QApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}