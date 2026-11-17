#include "app/connection_frontend.h"

#include <utility>

namespace hailstone::app {

bool connection_frontend(net::GameConnection& connection, std::optional<std::string>& pending_skin,
                         const std::function<void(const std::string&)>& load_skin,
                         const std::function<void(net::GameConnection::QuitRequest)>& quit,
                         const std::function<void()>& disconnected) {
    if (pending_skin) load_skin(*std::exchange(pending_skin, std::nullopt));
    if (const auto request = connection.take_quit_request(); request != net::GameConnection::QuitRequest::None) {
        quit(request);
        return false;
    }
    if (connection.state() == net::ConnectionState::Disconnected) {
        disconnected();
        return false;
    }
    return true;
}

} // namespace hailstone::app
