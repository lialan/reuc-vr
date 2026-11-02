// The client/updater coordination off Windows: there is no updater on these platforms yet, so a game session always starts and nothing can
// reserve the installation for an update.
#include "client_update_lock.h"

namespace uc {
struct ClientLockState {};

ClientSessionGuard::ClientSessionGuard(std::unique_ptr<ClientLockState> state) : state_(std::move(state)) {}
ClientSessionGuard::~ClientSessionGuard() = default;
ClientSessionGuard::ClientSessionGuard(ClientSessionGuard&&) noexcept = default;
ClientSessionGuard& ClientSessionGuard::operator=(ClientSessionGuard&&) noexcept = default;

ClientInstallGuard::ClientInstallGuard(std::unique_ptr<ClientLockState> state) : state_(std::move(state)) {}
ClientInstallGuard::~ClientInstallGuard() = default;
ClientInstallGuard::ClientInstallGuard(ClientInstallGuard&&) noexcept = default;
ClientInstallGuard& ClientInstallGuard::operator=(ClientInstallGuard&&) noexcept = default;
std::error_code ClientInstallGuard::wait_until_unused(std::chrono::milliseconds) { return std::make_error_code(std::errc::not_supported); }
std::error_code ClientInstallGuard::allow_child_start() { return std::make_error_code(std::errc::not_supported); }
std::error_code ClientInstallGuard::reacquire_for_rollback(std::chrono::milliseconds) { return std::make_error_code(std::errc::not_supported); }
void ClientInstallGuard::release_after_commit() {}
std::variant<std::uintptr_t, std::error_code> ClientInstallGuard::resume_channel(const std::string&) { return std::make_error_code(std::errc::not_supported); }

std::variant<ClientSessionGuard, std::error_code> enter_client_session(const std::filesystem::path&, const std::optional<ResumeTicket>&) {
    return ClientSessionGuard(std::make_unique<ClientLockState>());
}
std::variant<ClientInstallGuard, std::error_code> reserve_client_install(const std::filesystem::path&) {
    return std::make_error_code(std::errc::not_supported);
}
}  // namespace uc
