// scram_verifier.h: the client half of setting a password.
//
// Core stores a SCRAM-SHA-256 verifier and never receives the password, not
// at the bootstrap and not at a reset. So phi-cli does what the browser does:
// pick a salt, run PBKDF2, and send only the two keys the server keeps. The
// unix socket would carry a password safely enough, but core would then have
// two ways a password arrives, and only one of them is the one it was built
// around.

#pragma once

#include <scram.h>

#include <string>

namespace phicore::cli {

/// PBKDF2 rounds for a new verifier: core's own default, which is also what
/// the UI uses.
inline constexpr int kScramIterations = scram::kDefaultIterations;

/// The fields of a verifier as `cmd.users.password.set` takes them, each
/// base64 encoded.
struct ScramVerifier {
    std::string salt;
    int iterations = 0;
    std::string storedKey;
    std::string serverKey;
};

/// Derive a verifier for `password` with a fresh random salt. False, with the
/// reason in `errorOut`, only when OpenSSL fails.
bool deriveScramVerifier(const std::string &password,
                         int iterations,
                         ScramVerifier *out,
                         std::string *errorOut);

} // namespace phicore::cli
