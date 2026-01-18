#include "gui/main_window.hpp"
#include "gui/worker.hpp"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QGroupBox>
#include <QLabel>
// #include <QCheckBox>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setupUi();
}

MainWindow::~MainWindow() {}

void MainWindow::setupUi() {
    setWindowIcon(QIcon(":/app_logo"));

    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);

    QLabel *logoLabel = new QLabel(this);
    QPixmap logoPixmap(":/app_logo");
    
    if (!logoPixmap.isNull()) {
        logoLabel->setPixmap(logoPixmap.scaledToHeight(160, Qt::SmoothTransformation));
        
        logoLabel->setAlignment(Qt::AlignCenter);
        // Add some breathing room (top/bottom padding)
        logoLabel->setContentsMargins(0, 20, 0, 20); 
        
        mainLayout->insertWidget(0, logoLabel); 
    }

    // 0. Top Level Toggle
    m_jsonOnlyCheck = new QCheckBox("Mode: Generate intermediate JSON only (skip XML generation)", this);
    m_jsonOnlyCheck->setStyleSheet("font-weight: bold; margin: 5px;");
    connect(m_jsonOnlyCheck, &QCheckBox::toggled, this, &MainWindow::onJsonModeToggled);
    mainLayout->addWidget(m_jsonOnlyCheck);

    // Group 1: Mandatory Tax Data (XML specific)
    m_mandatoryGroup = new QGroupBox("Tax Data (Required for XML)", this);
    QFormLayout *formLayout = new QFormLayout(m_mandatoryGroup);

    m_taxNumEdit = new QLineEdit(this);
    m_taxNumEdit->setPlaceholderText("e.g. 12345678");
    
    m_yearSpin = new QSpinBox(this);
    m_yearSpin->setRange(2020, 2050);
    m_yearSpin->setValue(2025);

    m_formTypeCombo = new QComboBox(this);
    m_formTypeCombo->addItem("Doh-KDVP (Capital Gains)", 0);
    m_formTypeCombo->addItem("Doh-DIV (Dividends)", 1);
    m_formTypeCombo->addItem("Doh-DHO (Interest)", 2);

    m_docTypeCombo = new QComboBox(this);
    m_docTypeCombo->addItem("Original", static_cast<int>(FormType::Original));
    m_docTypeCombo->addItem("Self-Report (Samoprijava)", static_cast<int>(FormType::SelfReport));

    formLayout->addRow("Tax Number:", m_taxNumEdit);
    formLayout->addRow("Tax Year:", m_yearSpin);
    formLayout->addRow("Form Type:", m_formTypeCombo);
    formLayout->addRow("Document Type:", m_docTypeCombo);

    // Group 2: File Paths (Always required)
    QGroupBox *fileGroup = new QGroupBox("Input / Output", this);
    QGridLayout *fileLayout = new QGridLayout(fileGroup);

    m_inputFileEdit = new QLineEdit(this);
    QPushButton *browseInputBtn = new QPushButton("Browse...", this);
    connect(browseInputBtn, &QPushButton::clicked, this, &MainWindow::onBrowseFile);

    m_outputDirEdit = new QLineEdit(this);
    QPushButton *browseDirBtn = new QPushButton("Browse...", this);
    connect(browseDirBtn, &QPushButton::clicked, this, &MainWindow::onBrowseOutputDir);

    fileLayout->addWidget(new QLabel("Input File (PDF/JSON):"), 0, 0);
    fileLayout->addWidget(m_inputFileEdit, 0, 1);
    fileLayout->addWidget(browseInputBtn, 0, 2);

    fileLayout->addWidget(new QLabel("Output Directory:"), 1, 0);
    fileLayout->addWidget(m_outputDirEdit, 1, 1);
    fileLayout->addWidget(browseDirBtn, 1, 2);

    // Group 3: Optional Contact Info (XML specific)
    m_optGroup = new QGroupBox("Optional Contact Info", this);
    QFormLayout *optLayout = new QFormLayout(m_optGroup);
    m_emailEdit = new QLineEdit(this);
    m_phoneEdit = new QLineEdit(this);
    optLayout->addRow("Email:", m_emailEdit);
    optLayout->addRow("Phone:", m_phoneEdit);

    // Actions & Progress
    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    m_progressBar->setRange(0, 100);

    m_generateBtn = new QPushButton("Generate XML", this);
    m_generateBtn->setMinimumHeight(40);
    connect(m_generateBtn, &QPushButton::clicked, this, &MainWindow::onGenerateClicked);

    // Final Assembly
    mainLayout->addWidget(m_mandatoryGroup);
    mainLayout->addWidget(fileGroup);
    mainLayout->addWidget(m_optGroup);
    mainLayout->addStretch(); 
    mainLayout->addWidget(m_progressBar);
    mainLayout->addWidget(m_generateBtn);
    
    setWindowTitle("Edavki XML Maker GUI");
    resize(550, 650);
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