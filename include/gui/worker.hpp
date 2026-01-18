#pragma once
#include <QObject>
#include <QString>
#include "application_service.hpp"

class Worker : public QObject {
    Q_OBJECT // For signals and slots

public:
    explicit Worker(GenerationRequest request, QObject *parent = nullptr);

public slots:
    void process();

signals:
    void finished(bool success, QString message);
    void progressUpdated(int value);

private:
    GenerationRequest m_request;
};