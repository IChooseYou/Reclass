#pragma once
#include <QObject>
#include <QJsonObject>
#include <memory>

namespace rcx {
class RcxController;

// Native evidence collection, accessed through the existing MCP bridge.
// Jobs outlive client disconnects, but never survive a source/document change.
class TypeResolutionService : public QObject {
public:
    explicit TypeResolutionService(QObject* parent = nullptr);
    ~TypeResolutionService() override;
    QJsonObject analyze(const QJsonObject& args, RcxController* controller);
    QJsonObject apply(const QJsonObject& args, RcxController* controller);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
} // namespace rcx
