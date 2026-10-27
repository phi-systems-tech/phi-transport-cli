// scram_verifier.cpp: RFC 5802 / RFC 7677, the parts a client stores.

#include "scram_verifier.h"

#include <scram.h>

#include <phi/runtime/str.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>

namespace phicore::cli {

namespace {

constexpr int kSaltBytes = 16;  // what the UI uses; core wants at least that

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

    std::array<unsigned char, kSaltBytes> saltBytes{};
    if (RAND_bytes(saltBytes.data(), static_cast<int>(saltBytes.size())) != 1)
        return fail("no random salt");
    const std::string salt(reinterpret_cast<const char *>(saltBytes.data()), saltBytes.size());

    // The derivation is core's own (phi-transport-api scram.h), so the two
    // sides cannot drift apart.
    std::string salted = scram::saltedPassword(password, salt, iterations);
    if (salted.empty())
        return fail("PBKDF2 failed");
    std::string clientKey = scram::clientKey(salted);
    const std::string storedKey = scram::storedKey(clientKey);
    const std::string serverKey = scram::serverKey(salted);

    // The salted password and the client key are what a login proof is made
    // from, so neither outlives this call.
    OPENSSL_cleanse(salted.data(), salted.size());
    OPENSSL_cleanse(clientKey.data(), clientKey.size());
    if (storedKey.size() != scram::kKeyBytes || serverKey.size() != scram::kKeyBytes)
        return fail("HMAC failed");

    out->salt = phi::str::toBase64(salt);
    out->iterations = iterations;
    out->storedKey = phi::str::toBase64(storedKey);
    out->serverKey = phi::str::toBase64(serverKey);
    return true;
}

} // namespace phicore::cli
