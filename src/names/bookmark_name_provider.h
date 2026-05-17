#pragma once

#include "name_provider.h"
#include <functional>

namespace rcx {

class RcxController;

// Surfaces the active tab's per-document bookmarks (RcxDocument::tree.bookmarks)
// in the unified Symbols panel. Bookmarks are user-created labels — typed in
// via "Add Bookmark" dialog or the editor's right-click "Label this address..."
// — and they live in the .rcx project file across sessions.
//
// Because bookmarks are per-document, the provider takes a callback that
// returns the active controller. MainWindow supplies one that looks up the
// current tab.
class BookmarkNameProvider : public NameProvider {
public:
    using ActiveCtrlFn = std::function<RcxController*()>;

    explicit BookmarkNameProvider(ActiveCtrlFn fn) : m_fn(std::move(fn)) {}

    QString id() const override { return QStringLiteral("bookmark"); }
    QString displayName() const override { return QStringLiteral("Bookmarks"); }
    uint32_t accent() const override;

    QVector<NamedAddress> entries(const Provider* active) const override;
    bool supportsAdd() const override { return true; }
    bool add(const QString& name, uint64_t address) override;
    bool supportsRemove() const override { return true; }
    bool remove(const QString& name) override;

private:
    ActiveCtrlFn m_fn;
};

} // namespace rcx
