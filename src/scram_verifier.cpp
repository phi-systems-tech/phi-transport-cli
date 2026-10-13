// scram_verifier.cpp: RFC 5802 / RFC 7677, the parts a client stores.

#include "scram_verifier.h"

#include <phi/runtime/str.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstring>

namespace phicore::cli {

namespace {

constexpr int kKeyBytes = 32;   // SHA-256
constexpr int kSaltBytes = 16;  // what the UI uses; core wants at least that

using Key = std::array<unsigned char, kKeyBytes>;

bool hmacSha256(const Key &key, const char *message, Key *out)
{
    unsigned int len = 0;
    const unsigned char *result = HMAC(EVP_sha256(), key.data(), key.size(),
                                       reinterpret_cast<const unsigned char *>(message),
                                       std::strlen(message), out->data(), &len);
    return result != nullptr && len == kKeyBytes;
}

std::string toBase64(const unsigned char *bytes, std::size_t size)
{
    return phi::str::toBase64(std::string_view(reinterpret_cast<const char *>(bytes), size));
}

} // namespace

bool deriveScramVerifier(const std::string &password,
                         int iterations,
                         ScramVerifier *out,
                         std::string *errorOut)
{
    const auto fail = [errorOut](const char *what) {
        if (errorOut)
            *errorOut = std::string("Could not derive the verifier: ") + what;
        return false;
    };
    if (!out)
        return fail("no output");

    std::array<unsigned char, kSaltBytes> salt{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1)
        return fail("no random salt");

    // SaltedPassword := PBKDF2-HMAC-SHA-256(password, salt, i)
    Key salted{};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()), iterations,
                          EVP_sha256(), kKeyBytes, salted.data()) != 1) {
        return fail("PBKDF2 failed");
    }

    // StoredKey := SHA-256(HMAC(SaltedPassword, "Client Key"))
    // ServerKey := HMAC(SaltedPassword, "Server Key")
    Key clientKey{};
    Key storedKey{};
    Key serverKey{};
    const bool ok = hmacSha256(salted, "Client Key", &clientKey)
        && SHA256(clientKey.data(), clientKey.size(), storedKey.data()) != nullptr
        && hmacSha256(salted, "Server Key", &serverKey);

    // The salted password and the client key are what a login proof is made
    // from, so neither outlives this call.
    OPENSSL_cleanse(salted.data(), salted.size());
    OPENSSL_cleanse(clientKey.data(), clientKey.size());
    if (!ok)
        return fail("HMAC failed");

    out->salt = toBase64(salt.data(), salt.size());
    out->iterations = iterations;
    out->storedKey = toBase64(storedKey.data(), storedKey.size());
    out->serverKey = toBase64(serverKey.data(), serverKey.size());
    return true;
}

} // namespace phicore::cli
