#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QProgressBar>
#include <QPushButton>
#include <QCheckBox>
#include <QGroupBox> 
#include <QTabWidget>
#include <QTextBrowser>

#include "application_service.hpp"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onBrowseFile();
    void onBrowseOutputDir();
    void onGenerateClicked();
    void onWorkerFinished(bool success, QString message);
    void onJsonModeToggled(bool jsonOnly);

private:
    void setupUi();

    // tabs
    QWidget* createHomeTab();
    QWidget* createManualsTab();
    QWidget* createAboutTab();

    QTabWidget *m_mainTabs;

    // Group Boxes (stored as members to allow hiding/showing)
    QGroupBox *m_mandatoryGroup;
    QGroupBox *m_optGroup;

    // Control
    QCheckBox *m_jsonOnlyCheck;

    // Input Fields
    QLineEdit *m_taxNumEdit;
    QSpinBox  *m_yearSpin;
    QComboBox *m_formTypeCombo;
    QComboBox *m_docTypeCombo;
    QLineEdit *m_inputFileEdit;
    QLineEdit *m_outputDirEdit;
    QLineEdit *m_emailEdit;
    QLineEdit *m_phoneEdit;

    // Actions
    QProgressBar *m_progressBar;
    QPushButton  *m_generateBtn;
};