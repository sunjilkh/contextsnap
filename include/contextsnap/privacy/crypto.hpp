// Key management and at-rest encryption.
//
// Design: ContextSnap never asks the user to remember a passphrase. A 32-byte
// random data key is generated on first run and sealed by the platform secret
// store; SQLCipher receives it as a raw key. Export files can optionally be
// encrypted with AES-256-GCM using a passphrase-derived key (PBKDF2-HMAC-SHA256,
// 600k iterations, per-file random salt) so a snapshot can travel between
// machines without the OS keychain.
#pragma once

#include <contextsnap/core/result.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace contextsnap::privacy {

using core::Result;
using core::Status;

using Key = std::array<std::uint8_t, 32>;

/// Cryptographically secure random bytes (BCryptGenRandom / SecRandomCopyBytes /
/// getrandom(2)). Never falls back to a PRNG.
[[nodiscard]] Result<std::vector<std::uint8_t>> random_bytes(std::size_t count);
[[nodiscard]] Result<Key> random_key();

/// Constant-time comparison for MACs and key material.
[[nodiscard]] bool secure_equals(const std::vector<std::uint8_t>& a,
                                 const std::vector<std::uint8_t>& b) noexcept;

/// Best-effort zeroization that the optimizer may not remove.
void secure_zero(void* data, std::size_t length) noexcept;

/// Platform secret store: DPAPI (CryptProtectData) on Windows, Keychain on
/// macOS, Secret Service / kwallet on Linux with an encrypted-file fallback.
class Keyring {
public:
    virtual ~Keyring() = default;

    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> get(std::string_view account) = 0;
    [[nodiscard]] virtual Status set(std::string_view account,
                                     const std::vector<std::uint8_t>& secret) = 0;
    [[nodiscard]] virtual Status remove(std::string_view account) = 0;
    [[nodiscard]] virtual std::string backend_name() const = 0;

    [[nodiscard]] static Result<std::unique_ptr<Keyring>> create();
};

/// Fetches the database key, creating and sealing one on first use.
[[nodiscard]] Result<Key> get_or_create_database_key(Keyring& keyring);

/// Hex encoding used for the SQLCipher `PRAGMA key = "x'...'"` form.
[[nodiscard]] std::string to_hex(const std::vector<std::uint8_t>& bytes);
[[nodiscard]] std::string to_hex(const Key& key);
[[nodiscard]] Result<std::vector<std::uint8_t>> from_hex(std::string_view hex);

/// SHA-256 (OpenSSL when available, in-tree implementation otherwise).
[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::string_view data);
[[nodiscard]] std::string sha256_hex(std::string_view data);

/// PBKDF2-HMAC-SHA256. Requires CONTEXTSNAP_WITH_OPENSSL.
[[nodiscard]] Result<Key> derive_key(std::string_view passphrase,
                                     const std::vector<std::uint8_t>& salt,
                                     std::uint32_t iterations = 600000);

struct SealedBlob {
    std::vector<std::uint8_t> salt;
    std::vector<std::uint8_t> nonce;
    std::vector<std::uint8_t> ciphertext;
    std::vector<std::uint8_t> tag;

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
    [[nodiscard]] static Result<SealedBlob> from_bytes(const std::vector<std::uint8_t>& bytes);
};

/// AES-256-GCM encrypt/decrypt for export files. Requires OpenSSL.
[[nodiscard]] Result<SealedBlob> seal(std::string_view plaintext, std::string_view passphrase);
[[nodiscard]] Result<std::string> unseal(const SealedBlob& blob, std::string_view passphrase);

}  // namespace contextsnap::privacy
