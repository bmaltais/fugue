#include "node_kernel.h"

namespace fugue {

NodeKernel::~NodeKernel() = default;
ProviderKernel::~ProviderKernel() = default;

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

std::shared_ptr<ProviderKernel> Kernels::provider(const QString &typeId) const {
    const auto found = m_providers.find(typeId);
    return found == m_providers.end() ? nullptr : found->second;
}

} // namespace fugue
