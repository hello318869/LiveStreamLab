#include "AccountService.hpp"
#include "control_server/repositories/AccountRepository.hpp"

#include <sodium.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lsl::server {
namespace {

ApiError invalidCredentials() { return {401, "INVALID_CREDENTIALS", "Invalid username or password", {}}; }
ApiError unauthenticated() { return {401, "UNAUTHENTICATED", "Authentication required", {}}; }
ApiError busy() { return {503, "AUTHENTICATION_BUSY", "Authentication temporarily unavailable", {}}; }

struct Secret {
    explicit Secret(std::string value) : value(std::move(value)) {}
    ~Secret() { sodium_memzero(value.data(), value.size()); }
    std::string value;
};

// Only password hashing uses workers. Database calls remain asynchronous.
class PasswordWorkers {
public:
    PasswordWorkers() {
        try {
            for (unsigned i = 0; i < 2; ++i) {
                threads_.emplace_back([this] {
                    for (;;) {
                        std::function<void()> job;
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            condition_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
                            if (pending_.empty()) return;
                            job = std::move(pending_.front());
                            pending_.pop_front();
                        }
                        job();
                    }
                });
            }
        } catch (...) {
            stop();
            throw;
        }
    }

    ~PasswordWorkers() { stop(); }

    bool submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_ || pending_.size() >= 16) return false;
            pending_.push_back(std::move(job));
        }
        condition_.notify_one();
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> stopLock(stopMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            pending_.clear();
        }
        condition_.notify_all();
        for (auto& thread : threads_) if (thread.joinable()) thread.join();
    }

private:
    std::mutex mutex_;
    std::mutex stopMutex_;
    std::condition_variable condition_;
    bool stopping_ = false;
    std::deque<std::function<void()>> pending_;
    std::vector<std::thread> threads_;
};

bool tokenFormat(const std::string& token) {
    if (token.size() != 43) return false;
    for (const auto c : token) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    // Require canonical base64url: 32 bytes leave two padding bits in the last
    // character. Decode/re-encode rather than accepting aliases of one token.
    std::array<unsigned char, 32> bytes{};
    std::size_t length = 0;
    const char* end = nullptr;
    if (sodium_base642bin(bytes.data(), bytes.size(), token.data(), token.size(), nullptr,
                         &length, &end, sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
        length != bytes.size() || end != token.data() + token.size()) return false;
    std::array<char, 44> encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), bytes.data(), bytes.size(),
                      sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return token == encoded.data();
}

} // namespace

struct AccountService::Impl {
    explicit Impl(drogon::orm::DbClientPtr database, const ServerConfig& config)
        : repository(std::make_shared<AccountRepository>(std::move(database))),
          authTtl(config.lifecycle.auth_ttl_seconds) {}

    void ensureDummyHash() {
        std::call_once(dummyOnce, [this] {
            constexpr const char* dummyPassword = "not-an-account-password";
            if (crypto_pwhash_str_alg(dummyHash.data(), dummyPassword,
                                     std::char_traits<char>::length(dummyPassword),
                                     crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                     crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                     crypto_pwhash_ALG_ARGON2ID13) != 0) throw busy();
        });
    }

    std::shared_ptr<AccountRepository> repository;
    unsigned authTtl;
    std::atomic<bool> stopping{false};
    PasswordWorkers workers;
    std::once_flag dummyOnce;
    std::array<char, crypto_pwhash_STRBYTES> dummyHash{};
};

AccountService::AccountService(drogon::orm::DbClientPtr database, const ServerConfig& config)
    : impl_(std::make_shared<Impl>(std::move(database), config)) {}

AccountService::~AccountService() {
    // Join while this owner still exists, even if a queued job owns Impl.
    stop();
}

void AccountService::stop() {
    impl_->stopping.store(true);
    impl_->workers.stop();
}

void AccountService::registerAccount(Json::Value input, Reply reply, Fail fail) {
    const auto self = impl_;
    if (self->stopping.load()) { fail(busy()); return; }
    if (!self->repository->enabled()) { fail(databaseError()); return; }
    const auto username = requireUsername(input);
    const auto nickname = requireText(input, "nickname", 1, 32);
    auto password = std::make_shared<Secret>(requirePassword(input));
    input.clear();
    const bool accepted = self->workers.submit([self, username, nickname, password, reply, fail] {
        try {
            std::array<char, crypto_pwhash_STRBYTES> hash{};
            if (crypto_pwhash_str_alg(hash.data(), password->value.data(), password->value.size(),
                                     crypto_pwhash_OPSLIMIT_INTERACTIVE,
                                     crypto_pwhash_MEMLIMIT_INTERACTIVE,
                                     crypto_pwhash_ALG_ARGON2ID13) != 0) { fail(busy()); return; }
            std::string encodedHash(hash.data());
            sodium_memzero(hash.data(), hash.size());
            if (self->stopping.load()) { fail(busy()); return; }
            self->repository->createAccount(username, nickname, std::move(encodedHash), reply, fail);
        } catch (const ApiError& problem) { fail(problem); }
          catch (...) { fail(busy()); }
    });
    if (!accepted) fail(busy());
}

void AccountService::login(Json::Value input, Reply reply, Fail fail) {
    const auto self = impl_;
    if (self->stopping.load()) { fail(busy()); return; }
    if (!self->repository->enabled()) { fail(databaseError()); return; }
    const auto username = requireUsername(input);
    auto password = std::make_shared<Secret>(requirePassword(input));
    input.clear();
    self->repository->credentials(username,
        [self, password, reply, fail](Json::Value user, std::string hash, bool exists) {
            if (self->stopping.load()) { fail(busy()); return; }
            const bool accepted = self->workers.submit([self, password, exists, user, hash, reply, fail] {
                try {
                    self->ensureDummyHash();
                    const auto* encoded = exists ? hash.c_str() : self->dummyHash.data();
                    const bool verified = crypto_pwhash_str_verify(encoded, password->value.data(), password->value.size()) == 0;
                    if (self->stopping.load()) { fail(busy()); return; }
                    if (!exists || !verified) { fail(invalidCredentials()); return; }
                    const auto token = randomToken();
                    const auto sessionId = randomHex(16);
                    const auto digest = tokenHashHex(token);
                    const auto userId = std::stoull(user["id"].asString());
                    self->repository->createAuth(userId, sessionId, digest, self->authTtl,
                        [token, user, reply](Json::Value result) {
                            result["user"] = user;
                            result["access_token"] = token;
                            result["token_type"] = "Bearer";
                            reply(std::move(result));
                        }, fail);
                } catch (const ApiError& problem) { fail(problem); }
                  catch (...) { fail(busy()); }
            });
            if (!accepted) fail(busy());
        }, fail);
}

void AccountService::authenticate(std::string bearerToken,
                                  std::function<void(Principal)> reply, Fail fail) {
    if (!tokenFormat(bearerToken)) { fail(unauthenticated()); return; }
    impl_->repository->authenticate(tokenHashHex(bearerToken), std::move(reply), std::move(fail));
}

void AccountService::logout(const Principal& principal, Reply reply, Fail fail) {
    impl_->repository->revoke(principal, std::move(reply), std::move(fail));
}

void AccountService::me(const Principal& principal, Reply reply, Fail fail) {
    impl_->repository->me(principal, std::move(reply), std::move(fail));
}

void AccountService::room(const Principal& principal, Reply reply, Fail fail) {
    impl_->repository->room(principal, std::move(reply), std::move(fail));
}

void AccountService::patchRoom(const Principal& principal, Json::Value input, Reply reply, Fail fail) {
    if (input.empty()) throw ApiError{400, "INVALID_ARGUMENT", "At least one room field is required", {}};
    const bool titlePresent = input.isMember("title");
    const bool categoryPresent = input.isMember("category");
    const auto title = titlePresent ? requireText(input, "title", 1, 120) : std::string();
    const auto category = categoryPresent ? requireCategory(input) : std::string();
    impl_->repository->patchRoom(principal, titlePresent, title, categoryPresent, category,
                                 std::move(reply), std::move(fail));
}

} // namespace lsl::server
