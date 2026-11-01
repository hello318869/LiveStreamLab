#include "Support.hpp"

#include <arpa/inet.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <regex>
#include <string_view>
#include <vector>

namespace lsl::server {
namespace {

std::vector<std::uint32_t> scalars(const std::string& value) {
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i++]);
        std::uint32_t code = lead;
        unsigned continuation = 0;
        std::uint32_t minimum = 0;
        if (lead < 0x80) {}
        else if (lead >= 0xc2 && lead <= 0xdf) { code = lead & 0x1f; continuation = 1; minimum = 0x80; }
        else if (lead >= 0xe0 && lead <= 0xef) { code = lead & 0x0f; continuation = 2; minimum = 0x800; }
        else if (lead >= 0xf0 && lead <= 0xf4) { code = lead & 7; continuation = 3; minimum = 0x10000; }
        else throw invalidArgument();
        if (i + continuation > value.size()) throw invalidArgument();
        for (unsigned n = 0; n < continuation; ++n) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) throw invalidArgument();
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) throw invalidArgument();
        result.push_back(code);
    }
    return result;
}

bool whitespace(std::uint32_t cp) {
    return cp == 0x20 || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
        (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202f || cp == 0x205f || cp == 0x3000;
}

} // namespace

ApiError databaseError() { return {503, "DATABASE_UNAVAILABLE", "Database unavailable", {}}; }
ApiError internalError() { return {500, "INTERNAL_ERROR", "Internal server error", {}}; }
ApiError invalidArgument() { return {400, "INVALID_ARGUMENT", "Invalid request parameters", {}}; }

void beginTransaction(const drogon::orm::DbClientPtr& db,
    std::function<void(std::shared_ptr<drogon::orm::Transaction>)> callback, Fail fail) {
    if (!db) { fail(databaseError()); return; }
    try {
        db->newTransactionAsync([callback = std::move(callback), fail](const std::shared_ptr<drogon::orm::Transaction>& tx) {
            if (!tx) { fail(databaseError()); return; }
            try { callback(tx); }
            catch (const ApiError& error) { tx->rollback(); fail(error); }
            catch (...) { tx->rollback(); fail(internalError()); }
        });
    } catch (...) { fail(databaseError()); }
}

void commit(std::shared_ptr<drogon::orm::Transaction> tx, Reply done, Json::Value data, Fail fail) {
    tx->setCommitCallback([done = std::move(done), data = std::move(data), fail](bool ok) mutable {
        if (ok) done(std::move(data));
        else fail(databaseError());
    });
}

Json::Value strictBody(const drogon::HttpRequestPtr& request,
    std::initializer_list<const char*> allowed, std::initializer_list<const char*> required) {
    auto contentType = request->getHeader("content-type");
    const auto separator = contentType.find(';');
    contentType = contentType.substr(0, separator);
    while (!contentType.empty() && contentType.back() == ' ') contentType.pop_back();
    std::transform(contentType.begin(), contentType.end(), contentType.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (contentType != "application/json") throw ApiError{415, "UNSUPPORTED_MEDIA_TYPE", "JSON content required", {}};
    const auto body = request->body();
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["allowComments"] = false;
    builder["collectComments"] = false;
    builder["rejectDupKeys"] = true;
    builder["stackLimit"] = 32;
    auto reader = std::unique_ptr<Json::CharReader>(builder.newCharReader());
    Json::Value data;
    std::string errors;
    bool parsed = false;
    try { parsed = reader->parse(body.data(), body.data() + body.size(), &data, &errors); }
    catch (...) { throw invalidArgument(); }
    if (!parsed || !data.isObject()) throw invalidArgument();
    for (const auto& key : data.getMemberNames()) {
        if (std::none_of(allowed.begin(), allowed.end(), [&key](const char* name) { return key == name; })) throw invalidArgument();
    }
    for (const char* key : required) if (!data.isMember(key)) throw invalidArgument();
    return data;
}

std::string requireText(const Json::Value& object, const char* key, std::size_t min, std::size_t max) {
    if (!object[key].isString()) throw invalidArgument();
    const auto value = object[key].asString();
    const auto decoded = scalars(value);
    if (decoded.size() < min || decoded.size() > max ||
        std::any_of(decoded.begin(), decoded.end(), [](std::uint32_t cp) { return cp < 32 || (cp >= 127 && cp <= 159); }) ||
        std::all_of(decoded.begin(), decoded.end(), whitespace)) throw invalidArgument();
    return value;
}

std::string requireUsername(const Json::Value& object, const char* key) {
    const auto value = requireText(object, key, 3, 32);
    if (!std::regex_match(value, std::regex("[a-z0-9_]{3,32}"))) throw invalidArgument();
    return value;
}

std::string requirePassword(const Json::Value& object, const char* key) {
    if (!object[key].isString()) throw invalidArgument();
    const auto value = object[key].asString();
    if (value.size() < 8 || value.size() > 128) throw invalidArgument();
    (void)scalars(value);
    return value;
}

std::string requireCategory(const Json::Value& object, const char* key) {
    const auto value = requireText(object, key, 1, 16);
    if (value != "general" && value != "gaming" && value != "music" && value != "education" && value != "technology") throw invalidArgument();
    return value;
}

std::string requireUuid(const std::string& value) {
    if (!std::regex_match(value, std::regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"))) throw invalidArgument();
    return value;
}

std::string requireHexId(const std::string& value) {
    if (!std::regex_match(value, std::regex("[0-9a-f]{32}"))) throw invalidArgument();
    return value;
}

std::uint64_t requireDecimalId(const std::string& value) {
    if (value.empty() || value[0] == '0' || value.size() > 20) throw invalidArgument();
    std::uint64_t result = 0;
    for (const char c : value) {
        if (c < '0' || c > '9' || result > (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(c - '0')) / 10) throw invalidArgument();
        result = result * 10 + static_cast<unsigned>(c - '0');
    }
    return result;
}

std::string randomHex(std::size_t bytes) {
    std::vector<unsigned char> data(bytes);
    randombytes_buf(data.data(), data.size());
    std::string result(bytes * 2 + 1, '\0');
    sodium_bin2hex(result.data(), result.size(), data.data(), data.size());
    result.resize(bytes * 2);
    return result;
}

std::string randomToken() {
    std::array<unsigned char, 32> data{};
    randombytes_buf(data.data(), data.size());
    std::array<char, 44> encoded{};
    sodium_bin2base64(encoded.data(), encoded.size(), data.data(), data.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    return encoded.data();
}

std::string tokenHashHex(const std::string& token) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> data{};
    crypto_hash_sha256(data.data(), reinterpret_cast<const unsigned char*>(token.data()), token.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> result{};
    sodium_bin2hex(result.data(), result.size(), data.data(), data.size());
    return result.data();
}

bool constantTimeEqual(const std::string& left, const std::string& right) {
    return left.size() == right.size() && sodium_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::string canonicalIp(const std::string& value) {
    if (value.find('\0') != std::string::npos) return {};
    in_addr v4{};
    in6_addr v6{};
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    if (inet_pton(AF_INET, value.c_str(), &v4) == 1) {
        inet_ntop(AF_INET, &v4, buffer.data(), buffer.size());
        return buffer.data();
    }
    if (inet_pton(AF_INET6, value.c_str(), &v6) != 1) return {};
    if (IN6_IS_ADDR_V4MAPPED(&v6)) {
        std::copy(v6.s6_addr + 12, v6.s6_addr + 16, reinterpret_cast<unsigned char*>(&v4));
        inet_ntop(AF_INET, &v4, buffer.data(), buffer.size());
    } else inet_ntop(AF_INET6, &v6, buffer.data(), buffer.size());
    return buffer.data();
}

std::string peerIp(const drogon::HttpRequestPtr& request) { return canonicalIp(request->peerAddr().toIp()); }

std::string isoTime(std::string value) {
    if (value.empty()) return value;
    if (value.size() > 10 && value[10] == ' ') value[10] = 'T';
    if (value.back() != 'Z') value += 'Z';
    return value;
}

Json::Value jsonTime(const drogon::orm::Field& value) {
    return value.isNull() ? Json::Value() : Json::Value(isoTime(value.as<std::string>()));
}

Json::Value roomJson(const drogon::orm::Row& row) {
    Json::Value data(Json::objectValue);
    data["room_id"] = row["id"].as<std::string>();
    data["owner"]["id"] = row["owner_id"].as<std::string>();
    data["owner"]["nickname"] = row["nickname"].as<std::string>();
    data["title"] = row["title"].as<std::string>();
    data["category"] = row["category"].as<std::string>();
    data["cover_url"] = Json::Value();
    data["current_session_id"] = row["current_session_id"].isNull() ? Json::Value() : Json::Value(row["current_session_id"].as<std::string>());
    data["status"] = row["status"].isNull() ? "OFFLINE" : row["status"].as<std::string>();
    data["started_at"] = jsonTime(row["started_at"]);
    data["updated_at"] = jsonTime(row["updated_at"]);
    return data;
}

} // namespace lsl::server
