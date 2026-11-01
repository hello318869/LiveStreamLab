#include "control_server/config/ServerConfig.hpp"
#include "control_server/srs/SrsClient.hpp"

#include <drogon/HttpAppFramework.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Socket {
    int fd = -1;
    ~Socket() { if (fd >= 0) ::close(fd); }
};

// Model SRS's canonical collection routes without a real SRS process or DB.
// Requests without the trailing slash receive the actual 302 behavior which
// originally prevented an empty, healthy server from becoming AVAILABLE.
class SrsMock {
public:
    SrsMock() {
        listener_.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        require(listener_.fd >= 0, "cannot create mock socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(::bind(listener_.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
                "cannot bind mock socket");
        socklen_t size = sizeof(address);
        require(::getsockname(listener_.fd, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "cannot inspect mock port");
        port_ = ntohs(address.sin_port);
        require(::listen(listener_.fd, 4) == 0, "cannot listen on mock socket");
        worker_ = std::thread([this] {
            try { serve(); }
            catch (...) { if (!stopped_.load()) failure_ = std::current_exception(); }
        });
    }

    ~SrsMock() { stop(); }
    unsigned port() const { return port_; }

    void verify() {
        stop();
        if (failure_) std::rethrow_exception(failure_);
        if (paths_.size() != 3) {
            throw std::runtime_error("snapshot made " + std::to_string(paths_.size()) +
                " of 3 canonical API requests; last path=" + (paths_.empty() ? "none" : paths_.back()));
        }
        require(paths_[0] == "/api/v1/versions" && paths_[1] == "/api/v1/vhosts/",
                "snapshot used a noncanonical API route");
        require(paths_[2].substr(0, paths_[2].find('?')) == "/api/v1/streams/" &&
                    paths_[2].find("start=0") != std::string::npos &&
                    paths_[2].find("count=100") != std::string::npos,
                "snapshot collection route or pagination parameters are wrong");
    }

private:
    void stop() {
        stopped_.store(true);
        if (listener_.fd >= 0) ::shutdown(listener_.fd, SHUT_RDWR);
        if (worker_.joinable()) worker_.join();
    }

    void serve() {
        pollfd incoming{listener_.fd, POLLIN, 0};
        require(::poll(&incoming, 1, 3000) > 0, "mock timed out waiting for an API connection");
        if (stopped_.load()) return;
        Socket client{::accept(listener_.fd, nullptr, nullptr)};
        require(client.fd >= 0, "cannot accept mock connection");
        while (!stopped_.load() && paths_.size() < 3) {
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos) {
                pollfd readable{client.fd, POLLIN, 0};
                require(::poll(&readable, 1, 1000) > 0, "mock request header timed out");
                char buffer[1024];
                const auto count = ::recv(client.fd, buffer, sizeof(buffer), 0);
                require(count > 0, "mock request header ended prematurely");
                request.append(buffer, static_cast<std::size_t>(count));
                require(request.size() <= 8192, "mock request header is too large");
            }
            const auto begin = request.find(' ');
            const auto end = request.find(' ', begin == std::string::npos ? 0 : begin + 1);
            require(begin != std::string::npos && end != std::string::npos, "invalid mock request line");
            paths_.push_back(request.substr(begin + 1, end - begin - 1));
            const auto path = paths_.back().substr(0, paths_.back().find('?'));
            const std::string envelope = R"({"code":0,"server":"mock-server","service":"mock-service",)";
            std::string body;
            std::string status = "200 OK";
            std::string extra;
            if (path == "/api/v1/versions") {
                body = envelope + R"("data":{"version":"8.0.58","major":8,"minor":0,"revision":58}})";
            } else if (path == "/api/v1/vhosts/") {
                body = envelope + R"("vhosts":[]})";
            } else if (path == "/api/v1/streams/") {
                body = envelope + R"("total":0,"streams":[]})";
            } else if (path == "/api/v1/vhosts" || path == "/api/v1/streams") {
                status = "302 Found";
                extra = "Location: " + path + "/\r\n";
            } else {
                status = "404 Not Found";
            }
            const auto response = "HTTP/1.1 " + status + "\r\nContent-Type: application/json\r\n" +
                extra + "Content-Length: " + std::to_string(body.size()) +
                "\r\nConnection: keep-alive\r\n\r\n" + body;
            for (std::size_t sent = 0; sent < response.size();) {
                const auto count = ::send(client.fd, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
                require(count > 0, "cannot send mock response");
                sent += static_cast<std::size_t>(count);
            }
        }
    }

    Socket listener_;
    unsigned port_ = 0;
    std::atomic<bool> stopped_{false};
    std::thread worker_;
    std::exception_ptr failure_;
    std::vector<std::string> paths_;
};
} // namespace

int main() {
    try {
        SrsMock mock;
        lsl::server::ServerConfig config{};
        config.addresses.srs_api_internal_base = "http://127.0.0.1:" + std::to_string(mock.port());
        config.srs.expected_version = "8.0.58";
        config.srs.request_timeout_seconds = 1;
        auto& app = drogon::app();
        app.setLogLevel(trantor::Logger::kError);
        lsl::server::SrsClient client(config);
        bool succeeded = false;
        bool failed = false;
        bool timedOut = false;
        app.registerBeginningAdvice([&] {
            app.getLoop()->runAfter(4.0, [&] { timedOut = true; app.quit(); });
            client.snapshot([&](lsl::server::SrsSnapshot snapshot) {
                succeeded = snapshot.server == "mock-server" && snapshot.service == "mock-service" &&
                            snapshot.streams.empty();
                app.quit();
            }, [&] { failed = true; app.quit(); });
        });
        app.run();
        require(!timedOut, "snapshot exceeded its test deadline");
        mock.verify();
        require(succeeded && !failed, "snapshot failed against canonical SRS collection routes");
        std::cout << "PASS SRS snapshot follows canonical collection routes (3 requests)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL SRS client: " << error.what() << '\n';
        return 1;
    }
}
