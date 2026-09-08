#include <contextsnap/privacy/crypto.hpp>

#include <contextsnap/core/config.hpp>

#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#else
#include <sys/random.h>
#endif

#if defined(CONTEXTSNAP_WITH_OPENSSL)
#include <openssl/evp.h>
#endif

namespace contextsnap::privacy {
namespace {

constexpr char kAccount[] = "database-key";

// Compact FIPS 180-4 SHA-256 used when OpenSSL is not linked in.
struct Sha256 {
    std::array<std::uint32_t, 8> h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<std::uint8_t, 64> buf{};
    std::size_t len{0};
    std::uint64_t bits{0};

    static std::uint32_t ror(std::uint32_t v, std::uint32_t n) { return (v >> n) | (v << (32 - n)); }

    void block() {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t{buf[i * 4]} << 24) | (std::uint32_t{buf[i * 4 + 1]} << 16) |
                   (std::uint32_t{buf[i * 4 + 2]} << 8) | std::uint32_t{buf[i * 4 + 3]};
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto v = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) +
                                     ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
            const std::uint32_t t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) +
                                     ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) {
            h[i] += v[i];
        }
    }

    void update(std::string_view data) {
        bits += static_cast<std::uint64_t>(data.size()) * 8;
        for (const char c : data) {
            buf[len++] = static_cast<std::uint8_t>(c);
            if (len == 64) {
                block();
                len = 0;
            }
        }
    }

    std::array<std::uint8_t, 32> finish() {
        buf[len++] = 0x80;
        if (len > 56) {
            while (len < 64) buf[len++] = 0;
            block();
            len = 0;
        }
        while (len < 56) buf[len++] = 0;
        for (int i = 7; i >= 0; --i) {
            buf[len++] = static_cast<std::uint8_t>((bits >> (i * 8)) & 0xFF);
        }
        block();
        std::array<std::uint8_t, 32> out{};
        for (std::size_t i = 0; i < 8; ++i) {
            out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
        }
        return out;
    }
};

/// Portable fallback keyring: a 0600 file under the data directory. Used on
/// systems without a Secret Service provider; documented as weaker than the OS
/// keychain in docs/privacy-security.md.
class FileKeyring final : public Keyring {
public:
    Result<std::vector<std::uint8_t>> get(std::string_view account) override {
        std::ifstream file(path_for(account));
        if (!file.is_open()) {
            return core::err::not_found("no stored secret", "privacy.keyring");
        }
        std::string hex((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return from_hex(hex);
    }

    Status set(std::string_view account, const std::vector<std::uint8_t>& secret) override {
        const std::string path = path_for(account);
        if (const Status dir = core::paths::ensure_directory(
                std::filesystem::path(path).parent_path().string());
            !dir) {
            return dir;
        }
        std::ofstream file(path, std::ios::trunc);
        if (!file.is_open()) {
            return core::err::io("cannot write key file", "privacy.keyring");
        }
        file << to_hex(secret);
        file.close();
        std::error_code ec;
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
        return Status::success();
    }

    Status remove(std::string_view account) override {
        std::error_code ec;
        std::filesystem::remove(path_for(account), ec);
        return Status::success();
    }

    std::string backend_name() const override { return "file"; }

private:
    static std::string path_for(std::string_view account) {
        return (std::filesystem::path(core::paths::data_dir()) / "keys" / std::string(account))
            .string();
    }
};

}  // namespace

Result<std::vector<std::uint8_t>> random_bytes(std::size_t count) {
    std::vector<std::uint8_t> buffer(count);
    if (count == 0) {
        return buffer;
    }
#if defined(_WIN32)
    if (BCryptGenRandom(nullptr, buffer.data(), static_cast<ULONG>(count),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return core::err::internal("BCryptGenRandom failed", "privacy.random");
    }
#elif defined(__APPLE__)
    if (SecRandomCopyBytes(kSecRandomDefault, count, buffer.data()) != errSecSuccess) {
        return core::err::internal("SecRandomCopyBytes failed", "privacy.random");
    }
#else
    std::size_t filled = 0;
    while (filled < count) {
        const auto got = getrandom(buffer.data() + filled, count - filled, 0);
        if (got < 0) {
            return core::err::io("getrandom failed", "privacy.random", errno);
        }
        filled += static_cast<std::size_t>(got);
    }
#endif
    return buffer;
}

Result<Key> random_key() {
    auto bytes = random_bytes(32);
    if (!bytes) {
        return bytes.error();
    }
    Key key{};
    std::memcpy(key.data(), bytes.value().data(), key.size());
    return key;
}

bool secure_equals(const std::vector<std::uint8_t>& a,
                   const std::vector<std::uint8_t>& b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

void secure_zero(void* data, std::size_t length) noexcept {
    auto* p = static_cast<volatile std::uint8_t*>(data);
    while (length-- > 0) {
        *p++ = 0;
    }
}

Result<std::unique_ptr<Keyring>> Keyring::create() {
    return std::unique_ptr<Keyring>(new FileKeyring());
}

Result<Key> get_or_create_database_key(Keyring& keyring) {
    if (auto existing = keyring.get(kAccount); existing && existing.value().size() == 32) {
        Key key{};
        std::memcpy(key.data(), existing.value().data(), key.size());
        return key;
    }
    auto fresh = random_key();
    if (!fresh) {
        return fresh.error();
    }
    const std::vector<std::uint8_t> bytes(fresh.value().begin(), fresh.value().end());
    if (const Status stored = keyring.set(kAccount, bytes); !stored) {
        return stored.error();
    }
    return fresh;
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::string to_hex(const Key& key) {
    return to_hex(std::vector<std::uint8_t>(key.begin(), key.end()));
}

Result<std::vector<std::uint8_t>> from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        return core::err::invalid("hex string has odd length", "privacy.from_hex");
    }
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<std::uint8_t> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return core::err::invalid("invalid hex digit", "privacy.from_hex");
        }
        bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return bytes;
}

std::array<std::uint8_t, 32> sha256(std::string_view data) {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finish();
}

std::string sha256_hex(std::string_view data) {
    const auto digest = sha256(data);
    return to_hex(std::vector<std::uint8_t>(digest.begin(), digest.end()));
}

std::vector<std::uint8_t> SealedBlob::to_bytes() const {
    std::vector<std::uint8_t> out{'C', 'S', 'S', 'B', 1};
    out.push_back(static_cast<std::uint8_t>(salt.size()));
    out.push_back(static_cast<std::uint8_t>(nonce.size()));
    out.push_back(static_cast<std::uint8_t>(tag.size()));
    out.insert(out.end(), salt.begin(), salt.end());
    out.insert(out.end(), nonce.begin(), nonce.end());
    out.insert(out.end(), tag.begin(), tag.end());
    out.insert(out.end(), ciphertext.begin(), ciphertext.end());
    return out;
}

Result<SealedBlob> SealedBlob::from_bytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 8 || bytes[0] != 'C' || bytes[1] != 'S' || bytes[2] != 'S' ||
        bytes[3] != 'B') {
        return core::err::invalid("not a ContextSnap sealed blob", "privacy.sealed_blob");
    }
    if (bytes[4] != 1) {
        return core::err::unsupported("unsupported sealed blob version", "privacy.sealed_blob");
    }
    const std::size_t salt_len = bytes[5];
    const std::size_t nonce_len = bytes[6];
    const std::size_t tag_len = bytes[7];
    if (bytes.size() < 8 + salt_len + nonce_len + tag_len) {
        return core::err::invalid("truncated sealed blob", "privacy.sealed_blob");
    }
    SealedBlob blob;
    std::size_t offset = 8;
    blob.salt.assign(bytes.begin() + offset, bytes.begin() + offset + salt_len);
    offset += salt_len;
    blob.nonce.assign(bytes.begin() + offset, bytes.begin() + offset + nonce_len);
    offset += nonce_len;
    blob.tag.assign(bytes.begin() + offset, bytes.begin() + offset + tag_len);
    offset += tag_len;
    blob.ciphertext.assign(bytes.begin() + offset, bytes.end());
    return blob;
}

#if defined(CONTEXTSNAP_WITH_OPENSSL)

Result<Key> derive_key(std::string_view passphrase, const std::vector<std::uint8_t>& salt,
                       std::uint32_t iterations) {
    Key key{};
    if (PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()), salt.data(),
                          static_cast<int>(salt.size()), static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(key.size()), key.data()) != 1) {
        return core::err::internal("PBKDF2 derivation failed", "privacy.derive_key");
    }
    return key;
}

Result<SealedBlob> seal(std::string_view plaintext, std::string_view passphrase) {
    auto salt = random_bytes(16);
    auto nonce = random_bytes(12);
    if (!salt) return salt.error();
    if (!nonce) return nonce.error();
    auto key = derive_key(passphrase, salt.value(), 600000);
    if (!key) return key.error();

    SealedBlob blob;
    blob.salt = salt.value();
    blob.nonce = nonce.value();
    blob.ciphertext.resize(plaintext.size());
    blob.tag.resize(16);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int final_length = 0;
    bool ok = ctx != nullptr &&
              EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.value().data(),
                                 blob.nonce.data()) == 1 &&
              EVP_EncryptUpdate(ctx, blob.ciphertext.data(), &length,
                                reinterpret_cast<const unsigned char*>(plaintext.data()),
                                static_cast<int>(plaintext.size())) == 1 &&
              EVP_EncryptFinal_ex(ctx, blob.ciphertext.data() + length, &final_length) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, blob.tag.data()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return core::err::internal("AES-256-GCM seal failed", "privacy.seal");
    }
    return blob;
}

Result<std::string> unseal(const SealedBlob& blob, std::string_view passphrase) {
    auto key = derive_key(passphrase, blob.salt, 600000);
    if (!key) return key.error();

    std::string plaintext(blob.ciphertext.size(), '\0');
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int length = 0;
    int final_length = 0;
    std::vector<std::uint8_t> tag = blob.tag;
    bool ok = ctx != nullptr &&
              EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.value().data(),
                                 blob.nonce.data()) == 1 &&
              EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plaintext.data()), &length,
                                blob.ciphertext.data(),
                                static_cast<int>(blob.ciphertext.size())) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag.size()),
                                  tag.data()) == 1 &&
              EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plaintext.data()) + length,
                                  &final_length) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        return core::err::invalid("authentication failed: wrong passphrase or corrupt file",
                                  "privacy.unseal");
    }
    return plaintext;
}

#else

Result<Key> derive_key(std::string_view, const std::vector<std::uint8_t>&, std::uint32_t) {
    return core::err::unsupported("passphrase encryption requires -DCONTEXTSNAP_WITH_OPENSSL=ON",
                                  "privacy.derive_key");
}

Result<SealedBlob> seal(std::string_view, std::string_view) {
    return core::err::unsupported("AES-256-GCM requires -DCONTEXTSNAP_WITH_OPENSSL=ON",
                                  "privacy.seal");
}

Result<std::string> unseal(const SealedBlob&, std::string_view) {
    return core::err::unsupported("AES-256-GCM requires -DCONTEXTSNAP_WITH_OPENSSL=ON",
                                  "privacy.unseal");
}

#endif

}  // namespace contextsnap::privacy
