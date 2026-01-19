#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QGroupBox>
#include <QLabel>
#include <QFile>
#include <QTextStream>

#include "gui/main_window.hpp"
#include "gui/worker.hpp"


// Helper to read internal resource files
QString loadResource(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return "Error: Could not load content.";
    }
    QTextStream in(&file);
    return in.readAll();
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setupUi();
}

MainWindow::~MainWindow() {}

void MainWindow::setupUi() {
    setWindowIcon(QIcon(":/app_logo"));
    setWindowTitle("Edavki XML Maker");
    resize(600, 750);

    // Main Tab Widget
    m_mainTabs = new QTabWidget(this);
    setCentralWidget(m_mainTabs);

    // Tabs
    m_mainTabs->addTab(createHomeTab(), "Home");
    m_mainTabs->addTab(createManualsTab(), "Manuals");
    m_mainTabs->addTab(createAboutTab(), "About");
}

QWidget* MainWindow::createHomeTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);

    // --- Original Logic Starts Here ---

    QLabel *logoLabel = new QLabel(tab);
    QPixmap logoPixmap(":/app_logo");
    if (!logoPixmap.isNull()) {
        logoLabel->setPixmap(logoPixmap.scaledToHeight(120, Qt::SmoothTransformation));
        logoLabel->setAlignment(Qt::AlignCenter);
        logoLabel->setContentsMargins(0, 10, 0, 10);
        mainLayout->addWidget(logoLabel);
    }

    // 0. Top Level Toggle
    m_jsonOnlyCheck = new QCheckBox("Mode: Generate intermediate JSON only", tab);
    m_jsonOnlyCheck->setToolTip("Skip XML generation. Useful for debugging or checking parsed data.");
    m_jsonOnlyCheck->setStyleSheet("font-weight: bold; margin: 5px;");
    connect(m_jsonOnlyCheck, &QCheckBox::toggled, this, &MainWindow::onJsonModeToggled);
    mainLayout->addWidget(m_jsonOnlyCheck);

    // Group 1: Mandatory Tax Data
    m_mandatoryGroup = new QGroupBox("Tax Data (Required for XML)", tab);
    QFormLayout *formLayout = new QFormLayout(m_mandatoryGroup);

    m_taxNumEdit = new QLineEdit(tab);
    m_taxNumEdit->setPlaceholderText("e.g. 12345678");
    
    m_yearSpin = new QSpinBox(tab);
    m_yearSpin->setRange(2020, 2050);
    m_yearSpin->setValue(2025);

    m_formTypeCombo = new QComboBox(tab);
    m_formTypeCombo->addItem("Doh-KDVP (Capital Gains)", 0);
    m_formTypeCombo->addItem("Doh-DIV (Dividends)", 1);
    m_formTypeCombo->addItem("Doh-DHO (Interest)", 2);

    m_docTypeCombo = new QComboBox(tab);
    m_docTypeCombo->addItem("Original", static_cast<int>(FormType::Original));
    m_docTypeCombo->addItem("Self-Report (Samoprijava)", static_cast<int>(FormType::SelfReport));

    formLayout->addRow("Tax Number:", m_taxNumEdit);
    formLayout->addRow("Tax Year:", m_yearSpin);
    formLayout->addRow("Form Type:", m_formTypeCombo);
    formLayout->addRow("Document Type:", m_docTypeCombo);

    // Group 2: File Paths
    QGroupBox *fileGroup = new QGroupBox("Input / Output", tab);
    QGridLayout *fileLayout = new QGridLayout(fileGroup);

    m_inputFileEdit = new QLineEdit(tab);
    QPushButton *browseInputBtn = new QPushButton("Browse...", tab);
    connect(browseInputBtn, &QPushButton::clicked, this, &MainWindow::onBrowseFile);

    m_outputDirEdit = new QLineEdit(tab);
    QPushButton *browseDirBtn = new QPushButton("Browse...", tab);
    connect(browseDirBtn, &QPushButton::clicked, this, &MainWindow::onBrowseOutputDir);

    fileLayout->addWidget(new QLabel("Input File (PDF or JSON):"), 0, 0);
    fileLayout->addWidget(m_inputFileEdit, 0, 1);
    fileLayout->addWidget(browseInputBtn, 0, 2);

    fileLayout->addWidget(new QLabel("Output Directory:"), 1, 0);
    fileLayout->addWidget(m_outputDirEdit, 1, 1);
    fileLayout->addWidget(browseDirBtn, 1, 2);

    // Group 3: Optional Contact Info
    m_optGroup = new QGroupBox("Optional Contact Info", tab);
    QFormLayout *optLayout = new QFormLayout(m_optGroup);
    m_emailEdit = new QLineEdit(tab);
    m_phoneEdit = new QLineEdit(tab);
    optLayout->addRow("Email:", m_emailEdit);
    optLayout->addRow("Phone:", m_phoneEdit);

    // Actions
    m_progressBar = new QProgressBar(tab);
    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 100);

    m_generateBtn = new QPushButton("Generate XML", tab);
    m_generateBtn->setMinimumHeight(40);
    connect(m_generateBtn, &QPushButton::clicked, this, &MainWindow::onGenerateClicked);

    mainLayout->addWidget(m_mandatoryGroup);
    mainLayout->addWidget(fileGroup);
    mainLayout->addWidget(m_optGroup);
    mainLayout->addStretch();
    mainLayout->addWidget(m_progressBar);
    mainLayout->addWidget(m_generateBtn);

    return tab;
}

QWidget* MainWindow::createManualsTab() {
    QTabWidget *manualTabs = new QTabWidget();
    
    // 1. English Manual
    QTextBrowser *engBrowser = new QTextBrowser();
    engBrowser->setOpenExternalLinks(true);

    // Text is in manual_eng.html
    engBrowser->setText(loadResource(":/manual_eng"));

    // 2. Slovene Manual
    QTextBrowser *sloBrowser = new QTextBrowser();
    sloBrowser->setOpenExternalLinks(true);

    // Text is in manual_eng.html
    sloBrowser->setText(loadResource(":/manual_slo"));

    manualTabs->addTab(engBrowser, "Eng");
    manualTabs->addTab(sloBrowser, "Slo");
    
    return manualTabs;
}

QWidget* MainWindow::createAboutTab() {
    QWidget *tab = new QWidget();
    QVBoxLayout *layout = new QVBoxLayout(tab);

    QLabel *infoLabel = new QLabel(tab);
    infoLabel->setOpenExternalLinks(true); // Crucial for clickable links
    infoLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    infoLabel->setWordWrap(true);

    // Text is in about.html
    infoLabel->setText(loadResource(":/about"));

    layout->addWidget(infoLabel);
    layout->addStretch(); // Push content to top

    return tab;
}

void MainWindow::onJsonModeToggled(bool jsonOnly) {
    // Hide or show XML-specific sections
    m_mandatoryGroup->setVisible(!jsonOnly);
    m_optGroup->setVisible(!jsonOnly);
    
    // Update button label to reflect the current mode
    m_generateBtn->setText(jsonOnly ? "Generate JSON" : "Generate XML");
}

void MainWindow::onBrowseFile() {
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Open Report"), "", 
        tr("Supported Formats (*.pdf *.json);;PDF Files (*.pdf);;JSON Files (*.json)"));
    
    if (!fileName.isEmpty()) {
        m_inputFileEdit->setText(fileName);
    }
}

void MainWindow::onBrowseOutputDir() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
    if (!dir.isEmpty()) {
        m_outputDirEdit->setText(dir);
    }
}

void MainWindow::onGenerateClicked() {
    bool jsonOnly = m_jsonOnlyCheck->isChecked();

    // 1. Validation
    if (m_inputFileEdit->text().isEmpty() || m_outputDirEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Validation Error", "Please select an input file and output directory.");
        return;
    }

    if (!jsonOnly && m_taxNumEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Validation Error", "Tax Number is required for XML generation.");
        return;
    }

    // 2. Prepare Request
    GenerationRequest request;
    request.jsonOnly = jsonOnly;
    request.inputFile = m_inputFileEdit->text().toStdString();
    request.outputDirectory = m_outputDirEdit->text().toStdString();

    if (!jsonOnly) {
        request.taxNumber = m_taxNumEdit->text().toStdString();
        request.year = m_yearSpin->value();
        request.formDocType = static_cast<FormType>(m_docTypeCombo->currentData().toInt());
        
        int typeIndex = m_formTypeCombo->currentIndex();
        if (typeIndex == 0) request.formType = TaxFormType::Doh_KDVP;
        else if (typeIndex == 1) request.formType = TaxFormType::Doh_DIV;
        else request.formType = TaxFormType::Doh_DHO;

        if (!m_emailEdit->text().isEmpty()) request.email = m_emailEdit->text().toStdString();
        if (!m_phoneEdit->text().isEmpty()) request.phone = m_phoneEdit->text().toStdString();
    }

    // 3. Setup Threading for Background Processing
    QThread *thread = new QThread;
    Worker *worker = new Worker(request);
    
    worker->moveToThread(thread);

    connect(thread, &QThread::started, worker, &Worker::process);
    connect(worker, &Worker::finished, this, &MainWindow::onWorkerFinished);
    
    // Auto-cleanup when finished
    connect(worker, &Worker::finished, thread, &QThread::quit);
    connect(worker, &Worker::finished, worker, &Worker::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    // UI Feedback
    m_generateBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    
    thread->start();
}

void MainWindow::onWorkerFinished(bool success, QString message) {
    m_generateBtn->setEnabled(true);
    m_progressBar->setValue(100);
    m_progressBar->setVisible(false);

    if (success) {
        QMessageBox::information(this, "Success", "Processing completed successfully!\n\n" + message);
    } else {
        QMessageBox::critical(this, "Error", "An error occurred:\n" + message);
    }
}