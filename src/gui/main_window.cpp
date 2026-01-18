#include "gui/main_window.hpp"
#include "gui/worker.hpp"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QThread>
#include <QGroupBox>
#include <QLabel>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    setupUi();
}

MainWindow::~MainWindow() {}

void MainWindow::setupUi() {
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);

    // Group 1: Mandatory Data
    QGroupBox *mandatoryGroup = new QGroupBox("Mandatory Information", this);
    QFormLayout *formLayout = new QFormLayout(mandatoryGroup);

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

    // Group 2: File Paths
    QGroupBox *fileGroup = new QGroupBox("File Paths", this);
    QGridLayout *fileLayout = new QGridLayout(fileGroup);

    m_inputPdfEdit = new QLineEdit(this);
    QPushButton *browsePdfBtn = new QPushButton("Browse...", this);
    connect(browsePdfBtn, &QPushButton::clicked, this, &MainWindow::onBrowsePdf);

    m_outputDirEdit = new QLineEdit(this);
    QPushButton *browseDirBtn = new QPushButton("Browse...", this);
    connect(browseDirBtn, &QPushButton::clicked, this, &MainWindow::onBrowseOutputDir);

    fileLayout->addWidget(new QLabel("Input PDF:"), 0, 0);
    fileLayout->addWidget(m_inputPdfEdit, 0, 1);
    fileLayout->addWidget(browsePdfBtn, 0, 2);

    fileLayout->addWidget(new QLabel("Output Dir:"), 1, 0);
    fileLayout->addWidget(m_outputDirEdit, 1, 1);
    fileLayout->addWidget(browseDirBtn, 1, 2);

    // Group 3: Optional
    QGroupBox *optGroup = new QGroupBox("Optional Contact Info", this);
    QFormLayout *optLayout = new QFormLayout(optGroup);
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

    // Add to main layout
    mainLayout->addWidget(mandatoryGroup);
    mainLayout->addWidget(fileGroup);
    mainLayout->addWidget(optGroup);
    mainLayout->addWidget(m_progressBar);
    mainLayout->addWidget(m_generateBtn);
    
    setWindowTitle("Edavki XML Maker GUI");
    resize(500, 600);
}

void MainWindow::onBrowsePdf() {
    QString fileName = QFileDialog::getOpenFileName(this, "Select Tax Report PDF", "", "PDF Files (*.pdf)");
    if (!fileName.isEmpty()) {
        m_inputPdfEdit->setText(fileName);
    }
}

void MainWindow::onBrowseOutputDir() {
    QString dir = QFileDialog::getExistingDirectory(this, "Select Output Directory");
    if (!dir.isEmpty()) {
        m_outputDirEdit->setText(dir);
    }
}

void MainWindow::onGenerateClicked() {
    // 1. Validation
    if (m_taxNumEdit->text().isEmpty() || m_inputPdfEdit->text().isEmpty() || m_outputDirEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Validation Error", "Please fill in all mandatory fields.");
        return;
    }

    // 2. Prepare Request
    GenerationRequest request;
    request.taxNumber = m_taxNumEdit->text().toStdString();
    request.year = m_yearSpin->value();
    request.inputPdf = m_inputPdfEdit->text().toStdString();
    request.outputDirectory = m_outputDirEdit->text().toStdString();
    request.formDocType = static_cast<FormType>(m_docTypeCombo->currentData().toInt());
    
    int typeIndex = m_formTypeCombo->currentData().toInt();
    if (typeIndex == 0) request.formType = TaxFormType::Doh_KDVP;
    else if (typeIndex == 1) request.formType = TaxFormType::Doh_DIV;
    else request.formType = TaxFormType::Doh_DHO;

    if (!m_emailEdit->text().isEmpty()) request.email = m_emailEdit->text().toStdString();
    if (!m_phoneEdit->text().isEmpty()) request.phone = m_phoneEdit->text().toStdString();

    // 3. Setup Threading
    QThread *thread = new QThread;
    Worker *worker = new Worker(request);
    
    worker->moveToThread(thread);

    // Connect signals
    connect(thread, &QThread::started, worker, &Worker::process);
    connect(worker, &Worker::finished, this, &MainWindow::onWorkerFinished);
    
    // Cleanup logic
    connect(worker, &Worker::finished, thread, &QThread::quit);
    connect(worker, &Worker::finished, worker, &Worker::deleteLater);
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    // UI State
    m_generateBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0); // Indeterminate or start
    
    thread->start();
}

void MainWindow::onWorkerFinished(bool success, QString message) {
    m_generateBtn->setEnabled(true);
    m_progressBar->setValue(100);
    
    if (success) {
        QMessageBox::information(this, "Success", "Generation successful!\n" + message);
    } else {
        QMessageBox::critical(this, "Error", "Failed to generate XML:\n" + message);
    }
}