#include "node_kernel.h"

namespace fugue {

NodeKernel::~NodeKernel() = default;
Errors NodeKernel::check(const NodeContext &) const { return {}; }
QStringList NodeKernel::lazyInputs(const NodeType &type, const Values &) const {
    QStringList all;
    for (const InputSpec &input : type.inputs)
        if (input.lazy)
            all << input.name;
    return all;
}
ProviderKernel::~ProviderKernel() = default;
Errors ProviderKernel::check(const NodeContext &) const { return {}; }
Result<std::optional<JobRequest>> ProviderKernel::next(const NodeContext &, const Selection &, const JobResult &) {
    return std::optional<JobRequest>();
}
void ProviderKernel::abandon(const NodeContext &) {}

void Kernels::add(const QString &typeId, std::shared_ptr<NodeKernel> kernel) {
    m_providers.erase(typeId);
    m_nodes[typeId] = std::move(kernel);
}

void Kernels::add(const QString &typeId, std::shared_ptr<ProviderKernel> kernel) {
    m_nodes.erase(typeId);
    m_providers[typeId] = std::move(kernel);
}

std::shared_ptr<NodeKernel> Kernels::node(const QString &typeId) const {
    const auto found = m_nodes.find(typeId);
    return found == m_nodes.end() ? nullptr : found->second;
}

namespace {
QString keyAt(const QString &typeId, int major) { return QStringLiteral("%1@%2").arg(typeId).arg(major); }
} // namespace

void Kernels::addAt(const QString &typeId, int major, std::shared_ptr<NodeKernel> kernel) {
    add(keyAt(typeId, major), std::move(kernel));
}

void Kernels::addAt(const QString &typeId, int major, std::shared_ptr<ProviderKernel> kernel) {
    add(keyAt(typeId, major), std::move(kernel));
}

std::shared_ptr<NodeKernel> Kernels::node(const NodeType &type) const {
    if (std::shared_ptr<NodeKernel> older = node(keyAt(type.id, type.version.major)))
        return older;
    if (std::shared_ptr<NodeKernel> own = node(type.id))
        return own;
    return type.runtime.value("kind").toString() == QLatin1String("command") ? m_command : nullptr;
}

std::shared_ptr<ProviderKernel> Kernels::provider(const QString &typeId) const {
    const auto found = m_providers.find(typeId);
    return found == m_providers.end() ? nullptr : found->second;
}

std::shared_ptr<ProviderKernel> Kernels::provider(const NodeType &type) const {
    if (std::shared_ptr<ProviderKernel> older = provider(keyAt(type.id, type.version.major)))
        return older;
    return provider(type.id);
}

} // namespace fugue
