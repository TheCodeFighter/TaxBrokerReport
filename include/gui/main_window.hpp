#pragma once
#include <QMainWindow>
#include <QLineEdit>
#include <QComboBox>
#include <QSpinBox>
#include <QProgressBar>
#include <QPushButton>
#include <QCheckBox>

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

private:
    void setupUi();

    QLineEdit *m_taxNumEdit;
    QSpinBox *m_yearSpin;
    QComboBox *m_formTypeCombo;
    QLineEdit *m_inputFileEdit;
    QLineEdit *m_outputDirEdit;
    QLineEdit *m_emailEdit;
    QLineEdit *m_phoneEdit;
    QProgressBar *m_progressBar;
    QPushButton *m_generateBtn;
    QComboBox *m_docTypeCombo;
    QCheckBox *m_jsonOnlyCheck;
};