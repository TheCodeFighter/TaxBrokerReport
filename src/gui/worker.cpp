#include "worker.hpp"
#include "application_service.hpp"

Worker::Worker(GenerationRequest request, QObject *parent)
    : QObject(parent), m_request(std::move(request)) {}

void Worker::process() {
    emit progressUpdated(10);
    ApplicationService service;
    GenerationResult result = service.processRequest(m_request);
    emit progressUpdated(100);

    if (result.success) {
        QString msg = "Created:\n";
        for (const auto& file : result.createdFiles) {
            msg += QString::fromStdString(file.filename().string()) + "\n";
        }
        emit finished(true, msg);
    } else {
        emit finished(false, QString::fromStdString(result.message));
    }
}