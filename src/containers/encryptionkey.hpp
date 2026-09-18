#pragma once
/** ---------------------------------------------------------------------------------------------------------- Encryption Key
 * @file encryptionkey.hpp
 * @brief `EncryptionKey` - context that may hold any subset of an AES-256
 * symmetric key, an ED25519 keypair (or just a public half), and an X25519
 * keypair (or just a public half). All cryptographic methods are inline.
 * Operations whose key material is missing throw `EncryptionException`.
 */
#include <logging.hpp>
#include <alligator.hpp>
#include <openssl/cmac.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>

namespace buffetalligator {
/** ---------------------------------------------------------------------------------------------------------- EncryptionException
 * @class EncryptionException
 */
class EncryptionException : public threadsafe_logger::Exception {
public:
    using threadsafe_logger::Exception::Exception;
    using threadsafe_logger::Exception::what;
};
#define ENCRYPTION_THROW(msg) throw EncryptionException(msg)
/** ---------------------------------------------------------------------------------------------------------- KeyMaterialType
 * @enum KeyMaterialType
 */
enum class KeyMaterialType : uint8_t {
    AES_256,                                            ///< 32-byte symmetric key
    ED25519_PRIVATE,                                    ///< 32-byte ed25519 seed (derives public)
    ED25519_PUBLIC,                                     ///< 32-byte ed25519 public key
    X25519_PRIVATE,                                     ///< 32-byte x25519 private key (for ECIES decrypt)
    X25519_PUBLIC                                       ///< 32-byte x25519 public key (for ECIES encrypt)
};
/** ---------------------------------------------------------------------------------------------------------- EncryptionKey
 * @struct EncryptionKey
 * @brief Move-only key context. Holds whatever subset of key material the
 * caller imports; throws on operations that need material it doesn't have.
 */
struct EncryptionKey {
private:
    std::array<uint8_t, 32> aes_key_{};                 ///< AES-256 symmetric key
    bool has_aes_{false};
    EVP_PKEY* ed_priv_{nullptr};                        ///< ED25519 private (signing)
    EVP_PKEY* ed_pub_{nullptr};                         ///< ED25519 public (verify)
    EVP_PKEY* x_priv_{nullptr};                         ///< X25519 private (ECIES decrypt)
    EVP_PKEY* x_pub_{nullptr};                          ///< X25519 public (ECIES encrypt)
    /** --------------------------------------------------------------------------------- aead_overhead
     * @brief Bytes added by `encrypt()` framing: 12 nonce + 16 tag.
     */
    static constexpr size_t kAeadOverhead = 28;
    /** --------------------------------------------------------------------------------- ecies_overhead
     * @brief Bytes added by `pair_encrypt()` framing: 32 ephem-pub + 28 AEAD.
     */
    static constexpr size_t kEciesOverhead = 60;
    /** --------------------------------------------------------------------------------- derive_ed_pub_
     */
    static EVP_PKEY* derive_ed_pub_(EVP_PKEY* priv) {
        unsigned char pub[32];
        size_t publen = 32;
        if (EVP_PKEY_get_raw_public_key(priv, pub, &publen) <= 0) {
            ENCRYPTION_THROW("EVP_PKEY_get_raw_public_key (ed25519)");
        }
        EVP_PKEY* p = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub, 32);
        if (p == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_new_raw_public_key (ed25519)");
        }
        return p;
    }
    /** --------------------------------------------------------------------------------- derive_x_pub_
     */
    static EVP_PKEY* derive_x_pub_(EVP_PKEY* priv) {
        unsigned char pub[32];
        size_t publen = 32;
        if (EVP_PKEY_get_raw_public_key(priv, pub, &publen) <= 0) {
            ENCRYPTION_THROW("EVP_PKEY_get_raw_public_key (x25519)");
        }
        EVP_PKEY* p = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pub, 32);
        if (p == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_new_raw_public_key (x25519)");
        }
        return p;
    }
    /** --------------------------------------------------------------------------------- aes_gcm_encrypt_
     * @brief Common AES-256-GCM encrypt with explicit key + 12-byte nonce.
     */
    static void aes_gcm_encrypt_(
        const uint8_t* key32,
        const uint8_t* nonce12,
        const uint8_t* in,
        size_t in_len,
        uint8_t* cipher_out,
        uint8_t* tag_out_16
    ) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (ctx == nullptr) {
            ENCRYPTION_THROW("EVP_CIPHER_CTX_new");
        }
        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key32, nonce12) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_EncryptInit_ex");
        }
        int outl = 0;
        if (EVP_EncryptUpdate(ctx, cipher_out, &outl, in, static_cast<int>(in_len)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_EncryptUpdate");
        }
        int finall = 0;
        if (EVP_EncryptFinal_ex(ctx, cipher_out + outl, &finall) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_EncryptFinal_ex");
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag_out_16) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_CTRL_GCM_GET_TAG");
        }
        EVP_CIPHER_CTX_free(ctx);
    }
    /** --------------------------------------------------------------------------------- aes_gcm_decrypt_
     */
    static void aes_gcm_decrypt_(
        const uint8_t* key32,
        const uint8_t* nonce12,
        const uint8_t* cipher_in,
        size_t cipher_len,
        const uint8_t* tag_16,
        uint8_t* out
    ) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (ctx == nullptr) {
            ENCRYPTION_THROW("EVP_CIPHER_CTX_new");
        }
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key32, nonce12) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_DecryptInit_ex");
        }
        int outl = 0;
        if (EVP_DecryptUpdate(ctx, out, &outl, cipher_in, static_cast<int>(cipher_len)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_DecryptUpdate");
        }
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                const_cast<uint8_t*>(tag_16)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("EVP_CTRL_GCM_SET_TAG");
        }
        int finall = 0;
        if (EVP_DecryptFinal_ex(ctx, out + outl, &finall) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            ENCRYPTION_THROW("AEAD authentication failed");
        }
        EVP_CIPHER_CTX_free(ctx);
    }
    /** --------------------------------------------------------------------------------- hkdf_sha256_
     * @brief One-shot HKDF-SHA256 over `ikm` with the given `info` to produce 32 bytes.
     */
    static void hkdf_sha256_(
        const uint8_t* ikm,
        size_t ikm_len,
        const uint8_t* info,
        size_t info_len,
        uint8_t* out_32
    ) {
        EVP_PKEY_CTX* hctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
        if (hctx == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_CTX_new_id (HKDF)");
        }
        /// No-salt HKDF: the salt setter is intentionally never called - OpenSSL 3.6 rejects a
        /// null salt pointer outright, and RFC 5869 makes "salt absent" == zero-length salt.
        if (EVP_PKEY_derive_init(hctx) <= 0
            || EVP_PKEY_CTX_set_hkdf_md(hctx, EVP_sha256()) <= 0
            || EVP_PKEY_CTX_set1_hkdf_key(hctx, ikm, static_cast<int>(ikm_len)) <= 0
            || EVP_PKEY_CTX_add1_hkdf_info(hctx, info, static_cast<int>(info_len)) <= 0) {
            EVP_PKEY_CTX_free(hctx);
            ENCRYPTION_THROW("HKDF setup");
        }
        size_t outlen = 32;
        if (EVP_PKEY_derive(hctx, out_32, &outlen) <= 0) {
            EVP_PKEY_CTX_free(hctx);
            ENCRYPTION_THROW("EVP_PKEY_derive (HKDF)");
        }
        EVP_PKEY_CTX_free(hctx);
    }
    /** --------------------------------------------------------------------------------- aes_gcm_encrypt_method_
     * @brief Static AES-256-GCM encrypt body. Caller pre-allocates `in_len + 28`
     * bytes at `out`. Layout: `[12 nonce][in_len cipher][16 tag]`. Plumbed
     * through the `EncryptMethod` dispatch slot so the hot path is a single
     * function-pointer call with no per-record branching.
     */
    static size_t aes_gcm_encrypt_method_(
        const EncryptionKey& self,
        const uint8_t* in,
        size_t in_len,
        uint8_t* out
    ) {
        if (RAND_bytes(out, 12) != 1) [[unlikely]] {
            ENCRYPTION_THROW("RAND_bytes (nonce)");
        }
        aes_gcm_encrypt_(self.aes_key_.data(), out, in, in_len, out + 12, out + 12 + in_len);
        return in_len + kAeadOverhead;
    }
    /** --------------------------------------------------------------------------------- aes_gcm_decrypt_method_
     * @brief Static AES-256-GCM decrypt body. Plumbed through `DecryptMethod`.
     */
    static size_t aes_gcm_decrypt_method_(
        const EncryptionKey& self,
        const uint8_t* in,
        size_t in_len,
        uint8_t* out
    ) {
        // Layout: [12 nonce][cipher_len cipher][16 tag] - pass spans into `in` directly,
        // no defensive copy. Inner routine treats nonce/tag as `const uint8_t*` inputs.
        const size_t cipher_len = in_len - kAeadOverhead;
        aes_gcm_decrypt_(self.aes_key_.data(),
                         in,
                         in + 12, cipher_len,
                         in + 12 + cipher_len,
                         out);
        return cipher_len;
    }
    /** --------------------------------------------------------------------------------- free_
     */
    void free_() noexcept {
        if (ed_priv_ != nullptr) {
            EVP_PKEY_free(ed_priv_);
            ed_priv_ = nullptr;
        }
        if (ed_pub_ != nullptr) {
            EVP_PKEY_free(ed_pub_);
            ed_pub_ = nullptr;
        }
        if (x_priv_ != nullptr) {
            EVP_PKEY_free(x_priv_);
            x_priv_ = nullptr;
        }
        if (x_pub_ != nullptr) {
            EVP_PKEY_free(x_pub_);
            x_pub_ = nullptr;
        }
        has_aes_ = false;
    }
public:
    EncryptionKey() = default;
    EncryptionKey(const EncryptionKey&) = delete;
    EncryptionKey& operator=(const EncryptionKey&) = delete;
    /** --------------------------------------------------------------------------------- Aes Key Bytes
     * @brief Borrowed pointer to the raw 32-byte AES-256 key material. Callers (e.g.
     * the storage layer) need this to feed OpenSSL EVP directly. Lifetime: tied to
     * this EncryptionKey; the pointer is invalidated on move-out or destruction.
     */
    const uint8_t* aes_key_bytes() const noexcept {
        return aes_key_.data();
    }
    static constexpr uint32_t aes_key_size = 32u;
    /** --------------------------------------------------------------------------------- Move Constructor
     */
    EncryptionKey(EncryptionKey&& o) noexcept
    : aes_key_(o.aes_key_),
      has_aes_(o.has_aes_),
      ed_priv_(o.ed_priv_),
      ed_pub_(o.ed_pub_),
      x_priv_(o.x_priv_),
      x_pub_(o.x_pub_) {
        o.has_aes_ = false;
        o.ed_priv_ = nullptr;
        o.ed_pub_ = nullptr;
        o.x_priv_ = nullptr;
        o.x_pub_ = nullptr;
    }
    /** --------------------------------------------------------------------------------- Move Assignment
     */
    EncryptionKey& operator=(EncryptionKey&& o) noexcept {
        if (this != &o) {
            free_();
            aes_key_ = o.aes_key_;
            has_aes_ = o.has_aes_;
            ed_priv_ = o.ed_priv_;
            ed_pub_ = o.ed_pub_;
            x_priv_ = o.x_priv_;
            x_pub_ = o.x_pub_;
            o.has_aes_ = false;
            o.ed_priv_ = nullptr;
            o.ed_pub_ = nullptr;
            o.x_priv_ = nullptr;
            o.x_pub_ = nullptr;
        }
        return *this;
    }
    ~EncryptionKey() {
        free_();
    }
    /** --------------------------------------------------------------------------------- from_aes
     */
    static EncryptionKey from_aes(std::span<const uint8_t> key32) {
        if (key32.size() != 32) {
            ENCRYPTION_THROW("AES key must be 32 bytes");
        }
        EncryptionKey k;
        std::memcpy(k.aes_key_.data(), key32.data(), 32);
        k.has_aes_ = true;
        return k;
    }
    /** --------------------------------------------------------------------------------- from_ed25519_seed
     */
    static EncryptionKey from_ed25519_seed(std::span<const uint8_t> seed32) {
        if (seed32.size() != 32) {
            ENCRYPTION_THROW("ED25519 seed must be 32 bytes");
        }
        EncryptionKey k;
        k.ed_priv_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed32.data(), 32);
        if (k.ed_priv_ == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_new_raw_private_key (ed25519)");
        }
        k.ed_pub_ = derive_ed_pub_(k.ed_priv_);
        return k;
    }
    /** --------------------------------------------------------------------------------- from_ed25519_public
     */
    static EncryptionKey from_ed25519_public(std::span<const uint8_t> pub32) {
        if (pub32.size() != 32) {
            ENCRYPTION_THROW("ED25519 public key must be 32 bytes");
        }
        EncryptionKey k;
        k.ed_pub_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub32.data(), 32);
        if (k.ed_pub_ == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_new_raw_public_key (ed25519)");
        }
        return k;
    }
    /** --------------------------------------------------------------------------------- from_x25519_private
     */
    static EncryptionKey from_x25519_private(std::span<const uint8_t> priv32) {
        if (priv32.size() != 32) {
            ENCRYPTION_THROW("X25519 private key must be 32 bytes");
        }
        EncryptionKey k;
        k.x_priv_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, priv32.data(), 32);
        if (k.x_priv_ == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_new_raw_private_key (x25519)");
        }
        k.x_pub_ = derive_x_pub_(k.x_priv_);
        return k;
    }
    /** --------------------------------------------------------------------------------- from_x25519_public
     */
    static EncryptionKey from_x25519_public(std::span<const uint8_t> pub32) {
        if (pub32.size() != 32) {
            ENCRYPTION_THROW("X25519 public key must be 32 bytes");
        }
        EncryptionKey k;
        k.x_pub_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, pub32.data(), 32);
        if (k.x_pub_ == nullptr) {
            ENCRYPTION_THROW("EVP_PKEY_new_raw_public_key (x25519)");
        }
        return k;
    }
    /** --------------------------------------------------------------------------------- random_aes
     */
    static EncryptionKey random_aes() {
        EncryptionKey k;
        if (RAND_bytes(k.aes_key_.data(), 32) != 1) {
            ENCRYPTION_THROW("RAND_bytes");
        }
        k.has_aes_ = true;
        return k;
    }
    /** --------------------------------------------------------------------------------- random_ed25519
     */
    static EncryptionKey random_ed25519() {
        EncryptionKey k;
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) <= 0
            || EVP_PKEY_keygen(ctx, &k.ed_priv_) <= 0) {
            if (ctx != nullptr) {
                EVP_PKEY_CTX_free(ctx);
            }
            ENCRYPTION_THROW("EVP_PKEY_keygen (ed25519)");
        }
        EVP_PKEY_CTX_free(ctx);
        k.ed_pub_ = derive_ed_pub_(k.ed_priv_);
        return k;
    }
    /** --------------------------------------------------------------------------------- random_x25519
     */
    static EncryptionKey random_x25519() {
        EncryptionKey k;
        EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        if (ctx == nullptr || EVP_PKEY_keygen_init(ctx) <= 0
            || EVP_PKEY_keygen(ctx, &k.x_priv_) <= 0) {
            if (ctx != nullptr) {
                EVP_PKEY_CTX_free(ctx);
            }
            ENCRYPTION_THROW("EVP_PKEY_keygen (x25519)");
        }
        EVP_PKEY_CTX_free(ctx);
        k.x_pub_ = derive_x_pub_(k.x_priv_);
        return k;
    }
    /** --------------------------------------------------------------------------------- import_key
     * @brief Import additional key material into an existing context. Replaces
     * any prior material of the same kind. Public-key import does not erase a
     * matching private if present.
     */
    void import_key(KeyMaterialType type, std::span<const uint8_t> material) {
        if (material.size() != 32) {
            ENCRYPTION_THROW("import_key: material must be 32 bytes");
        }
        switch (type) {
            case KeyMaterialType::AES_256: {
                std::memcpy(aes_key_.data(), material.data(), 32);
                has_aes_ = true;
                break;
            }
            case KeyMaterialType::ED25519_PRIVATE: {
                if (ed_priv_ != nullptr) {
                    EVP_PKEY_free(ed_priv_);
                }
                ed_priv_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, material.data(), 32);
                if (ed_priv_ == nullptr) {
                    ENCRYPTION_THROW("import ed25519 private");
                }
                if (ed_pub_ == nullptr) {
                    ed_pub_ = derive_ed_pub_(ed_priv_);
                }
                break;
            }
            case KeyMaterialType::ED25519_PUBLIC: {
                if (ed_pub_ != nullptr) {
                    EVP_PKEY_free(ed_pub_);
                }
                ed_pub_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, material.data(), 32);
                if (ed_pub_ == nullptr) {
                    ENCRYPTION_THROW("import ed25519 public");
                }
                break;
            }
            case KeyMaterialType::X25519_PRIVATE: {
                if (x_priv_ != nullptr) {
                    EVP_PKEY_free(x_priv_);
                }
                x_priv_ = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, material.data(), 32);
                if (x_priv_ == nullptr) {
                    ENCRYPTION_THROW("import x25519 private");
                }
                if (x_pub_ == nullptr) {
                    x_pub_ = derive_x_pub_(x_priv_);
                }
                break;
            }
            case KeyMaterialType::X25519_PUBLIC: {
                if (x_pub_ != nullptr) {
                    EVP_PKEY_free(x_pub_);
                }
                x_pub_ = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, material.data(), 32);
                if (x_pub_ == nullptr) {
                    ENCRYPTION_THROW("import x25519 public");
                }
                break;
            }
        }
    }
    bool has_aes() const noexcept {
        return has_aes_;
    }
    bool has_ed25519_private() const noexcept {
        return ed_priv_ != nullptr;
    }
    bool has_ed25519_public() const noexcept {
        return ed_pub_ != nullptr;
    }
    bool has_x25519_private() const noexcept {
        return x_priv_ != nullptr;
    }
    bool has_x25519_public() const noexcept {
        return x_pub_ != nullptr;
    }
    /** --------------------------------------------------------------------------------- EncryptMethod
     * @brief Function-pointer signature for the encrypt dispatch slot.
     */
    using EncryptMethod = size_t(*)(const EncryptionKey&, const uint8_t*, size_t, uint8_t*);
    /** --------------------------------------------------------------------------------- DecryptMethod
     */
    using DecryptMethod = size_t(*)(const EncryptionKey&, const uint8_t*, size_t, uint8_t*);
    /** --------------------------------------------------------------------------------- get_encrypt_method
     * @brief Process-wide dispatch slot for the active encrypt path. Defaults
     * to AES-256-GCM (secure).
     */
    static EncryptMethod& get_encrypt_method() {
        static EncryptMethod method = aes_gcm_encrypt_method_;
        return method;
    }
    /** --------------------------------------------------------------------------------- get_decrypt_method
     */
    static DecryptMethod& get_decrypt_method() {
        static DecryptMethod method = aes_gcm_decrypt_method_;
        return method;
    }
    /** --------------------------------------------------------------------------------- encrypt
     * @brief AES-256-GCM (or no-op pass-through, depending on the dispatch slot).
     * Output layout: `[12 nonce][in_len cipher][16 tag]`. Total bytes written =
     * `in_len + 28`. Caller pre-allocates that much in `out` (non-null).
     */
    size_t encrypt(const uint8_t* in, size_t in_len, uint8_t* out) const {
        return get_encrypt_method()(*this, in, in_len, out);
    }
    /** --------------------------------------------------------------------------------- decrypt
     * @brief Inverse of `encrypt`. Plaintext bytes written = `in_len - 28`.
     */
    size_t decrypt(const uint8_t* in, size_t in_len, uint8_t* out) const {
        return get_decrypt_method()(*this, in, in_len, out);
    }
    /** --------------------------------------------------------------------------------- pair_encrypt
     * @brief ECIES via X25519 + AES-256-GCM. Requires `has_x25519_public()` -
     * the context's X25519 public key is the recipient. Output layout:
     * `[32 ephem_pub][12 nonce][in_len cipher][16 tag]`. Total = `in_len + 60`.
     */
    size_t pair_encrypt(const uint8_t* in, size_t in_len, uint8_t* out = nullptr) const {
        if (x_pub_ == nullptr) {
            ENCRYPTION_THROW("pair_encrypt: no X25519 public key (recipient)");
        }
        if (out == nullptr) {
            out = const_cast<uint8_t*>(in);
        }
        const bool in_place = (out == in);
        if (in_place) {
            std::memmove(out + 44, in, in_len);
        }
        EVP_PKEY_CTX* gctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
        EVP_PKEY* ephem = nullptr;
        if (gctx == nullptr || EVP_PKEY_keygen_init(gctx) <= 0
            || EVP_PKEY_keygen(gctx, &ephem) <= 0) {
            if (gctx != nullptr) {
                EVP_PKEY_CTX_free(gctx);
            }
            ENCRYPTION_THROW("pair_encrypt: ephemeral keygen");
        }
        EVP_PKEY_CTX_free(gctx);
        size_t ephem_pub_len = 32;
        if (EVP_PKEY_get_raw_public_key(ephem, out, &ephem_pub_len) <= 0) {
            EVP_PKEY_free(ephem);
            ENCRYPTION_THROW("pair_encrypt: get ephem pub");
        }
        EVP_PKEY_CTX* dctx = EVP_PKEY_CTX_new(ephem, nullptr);
        unsigned char shared[32];
        size_t shared_len = 32;
        if (dctx == nullptr || EVP_PKEY_derive_init(dctx) <= 0
            || EVP_PKEY_derive_set_peer(dctx, x_pub_) <= 0
            || EVP_PKEY_derive(dctx, shared, &shared_len) <= 0) {
            if (dctx != nullptr) {
                EVP_PKEY_CTX_free(dctx);
            }
            EVP_PKEY_free(ephem);
            ENCRYPTION_THROW("pair_encrypt: ECDH derive");
        }
        EVP_PKEY_CTX_free(dctx);
        EVP_PKEY_free(ephem);
        unsigned char derived[32];
        static constexpr uint8_t kInfo[] = "nebula-ecies/v1";
        hkdf_sha256_(shared, 32, kInfo, sizeof(kInfo) - 1, derived);
        if (RAND_bytes(out + 32, 12) != 1) {
            ENCRYPTION_THROW("RAND_bytes (ECIES nonce)");
        }
        const uint8_t* src = in_place ? (out + 44) : in;
        aes_gcm_encrypt_(derived, out + 32, src, in_len, out + 44, out + 44 + in_len);
        return in_len + kEciesOverhead;
    }
    /** --------------------------------------------------------------------------------- pair_decrypt
     * @brief Inverse of `pair_encrypt`. Requires `has_x25519_private()`.
     * Plaintext bytes written = `in_len - 60`.
     */
    size_t pair_decrypt(const uint8_t* in, size_t in_len, uint8_t* out = nullptr) const {
        if (x_priv_ == nullptr) {
            ENCRYPTION_THROW("pair_decrypt: no X25519 private key");
        }
        if (in_len < kEciesOverhead) {
            ENCRYPTION_THROW("pair_decrypt: ciphertext shorter than ECIES overhead");
        }
        if (out == nullptr) {
            out = const_cast<uint8_t*>(in);
        }
        const size_t cipher_len = in_len - kEciesOverhead;
        std::array<uint8_t, 32> ephem_pub_copy{};
        std::array<uint8_t, 12> nonce_copy{};
        std::array<uint8_t, 16> tag_copy{};
        std::memcpy(ephem_pub_copy.data(), in, 32);
        std::memcpy(nonce_copy.data(), in + 32, 12);
        std::memcpy(tag_copy.data(), in + 44 + cipher_len, 16);
        EVP_PKEY* ephem_pub = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr,
                                                          ephem_pub_copy.data(), 32);
        if (ephem_pub == nullptr) {
            ENCRYPTION_THROW("pair_decrypt: load ephem pub");
        }
        EVP_PKEY_CTX* dctx = EVP_PKEY_CTX_new(x_priv_, nullptr);
        unsigned char shared[32];
        size_t shared_len = 32;
        if (dctx == nullptr || EVP_PKEY_derive_init(dctx) <= 0
            || EVP_PKEY_derive_set_peer(dctx, ephem_pub) <= 0
            || EVP_PKEY_derive(dctx, shared, &shared_len) <= 0) {
            if (dctx != nullptr) {
                EVP_PKEY_CTX_free(dctx);
            }
            EVP_PKEY_free(ephem_pub);
            ENCRYPTION_THROW("pair_decrypt: ECDH derive");
        }
        EVP_PKEY_CTX_free(dctx);
        EVP_PKEY_free(ephem_pub);
        unsigned char derived[32];
        static constexpr uint8_t kInfo[] = "nebula-ecies/v1";
        hkdf_sha256_(shared, 32, kInfo, sizeof(kInfo) - 1, derived);
        aes_gcm_decrypt_(derived, nonce_copy.data(),
                         in + 44, cipher_len, tag_copy.data(), out);
        return cipher_len;
    }
    /** --------------------------------------------------------------------------------- sign_message
     * @brief ED25519 detached signature. `sig_64` must point to 64 bytes.
     */
    void sign_message(const uint8_t* msg, size_t msg_len, uint8_t* sig_64) const {
        if (ed_priv_ == nullptr) {
            ENCRYPTION_THROW("sign_message: no ED25519 private key");
        }
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (mdctx == nullptr) {
            ENCRYPTION_THROW("EVP_MD_CTX_new");
        }
        if (EVP_DigestSignInit(mdctx, nullptr, nullptr, nullptr, ed_priv_) != 1) {
            EVP_MD_CTX_free(mdctx);
            ENCRYPTION_THROW("EVP_DigestSignInit");
        }
        size_t siglen = 64;
        if (EVP_DigestSign(mdctx, sig_64, &siglen, msg, msg_len) != 1) {
            EVP_MD_CTX_free(mdctx);
            ENCRYPTION_THROW("EVP_DigestSign");
        }
        EVP_MD_CTX_free(mdctx);
    }
    /** --------------------------------------------------------------------------------- verify_signature
     * @brief ED25519 verify. Returns true iff the signature is valid for `msg`.
     */
    bool verify_signature(const uint8_t* msg, size_t msg_len, const uint8_t* sig_64) const {
        if (ed_pub_ == nullptr) {
            ENCRYPTION_THROW("verify_signature: no ED25519 public key");
        }
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (mdctx == nullptr) {
            ENCRYPTION_THROW("EVP_MD_CTX_new");
        }
        if (EVP_DigestVerifyInit(mdctx, nullptr, nullptr, nullptr, ed_pub_) != 1) {
            EVP_MD_CTX_free(mdctx);
            ENCRYPTION_THROW("EVP_DigestVerifyInit");
        }
        const int rc = EVP_DigestVerify(mdctx, sig_64, 64, msg, msg_len);
        EVP_MD_CTX_free(mdctx);
        return rc == 1;
    }
    /** --------------------------------------------------------------------------------- hmac_256
     * @brief HMAC-SHA256. `out_32` receives the full 32-byte tag.
     */
    void hmac_256(const uint8_t* in, size_t in_len, uint8_t* out_32) const {
        if (!has_aes_) {
            ENCRYPTION_THROW("hmac_256: no symmetric key");
        }
        EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
        if (mac == nullptr) {
            ENCRYPTION_THROW("EVP_MAC_fetch HMAC");
        }
        EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
        if (ctx == nullptr) {
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("EVP_MAC_CTX_new HMAC");
        }
        char digest[] = "SHA256";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
            OSSL_PARAM_construct_end()
        };
        if (EVP_MAC_init(ctx, aes_key_.data(), 32, params) != 1
            || EVP_MAC_update(ctx, in, in_len) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("HMAC init/update");
        }
        size_t outlen = 32;
        if (EVP_MAC_final(ctx, out_32, &outlen, 32) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("HMAC final");
        }
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
    }
    /** --------------------------------------------------------------------------------- cmac_256
     * @brief CMAC-AES-256. Uses the full 32-byte AES key as the cipher key.
     * @param in Input data.
     * @param in_len Length of the input data.
     * @param out_32 Output buffer for the 32-byte CMAC tag.
     */
    void cmac_256(const uint8_t* in, size_t in_len, uint8_t* out_32) const {
        if (!has_aes_) {
            ENCRYPTION_THROW("cmac_256: no symmetric key");
        }
        EVP_MAC* mac = EVP_MAC_fetch(nullptr, "CMAC", nullptr);
        if (mac == nullptr) {
            ENCRYPTION_THROW("EVP_MAC_fetch CMAC");
        }
        EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
        if (ctx == nullptr) {
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("EVP_MAC_CTX_new CMAC");
        }
        char cipher[] = "AES-256-CBC";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, cipher, 0),
            OSSL_PARAM_construct_end()
        };
        if (EVP_MAC_init(ctx, aes_key_.data(), 32, params) != 1
            || EVP_MAC_update(ctx, in, in_len) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("CMAC-256 init/update");
        }
        size_t outlen = 32;
        if (EVP_MAC_final(ctx, out_32, &outlen, 32) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("CMAC-256 final");
        }
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
    }
    /** --------------------------------------------------------------------------------- hmac_128
     * @brief HMAC-SHA256 truncated to 16 bytes.
     */
    void hmac_128(const uint8_t* in, size_t in_len, uint8_t* out_16) const {
        std::array<uint8_t, 32> full{};
        hmac_256(in, in_len, full.data());
        std::memcpy(out_16, full.data(), 16);
    }
    /** --------------------------------------------------------------------------------- cmac_128
     * @brief CMAC-AES-128. Uses the first 16 bytes of the AES key as the cipher key.
     */
    void cmac_128(const uint8_t* in, size_t in_len, uint8_t* out_16) const {
        if (!has_aes_) {
            ENCRYPTION_THROW("cmac_128: no symmetric key");
        }
        EVP_MAC* mac = EVP_MAC_fetch(nullptr, "CMAC", nullptr);
        if (mac == nullptr) {
            ENCRYPTION_THROW("EVP_MAC_fetch CMAC");
        }
        EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
        if (ctx == nullptr) {
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("EVP_MAC_CTX_new CMAC");
        }
        char cipher[] = "AES-128-CBC";
        OSSL_PARAM params[] = {
            OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, cipher, 0),
            OSSL_PARAM_construct_end()
        };
        if (EVP_MAC_init(ctx, aes_key_.data(), 16, params) != 1
            || EVP_MAC_update(ctx, in, in_len) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("CMAC-128 init/update");
        }
        size_t outlen = 16;
        if (EVP_MAC_final(ctx, out_16, &outlen, 16) != 1) {
            EVP_MAC_CTX_free(ctx);
            EVP_MAC_free(mac);
            ENCRYPTION_THROW("CMAC-128 final");
        }
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
    }
    /** --------------------------------------------------------------------------------- encrypted_size
     * @brief Returns the number of bytes that `encrypt()` will write for a given
     * plaintext length. This is the plaintext length + 28 bytes of framing.
     */
    static size_t encrypted_size(size_t plaintext_len) {
        return plaintext_len + kAeadOverhead;
    }
};
/** ---------------------------------------------------------------------------------------------------------- HashKey128 mac
 * @brief Out-of-line definition of the static `mac()` declared in `hash_keys.hpp`.
 * `mac` is no longer SFINAE-gated - the dispatch lives in `set_secure_hash_method`,
 * not in the type, so we always provide a definition for every `T`.
 */
template<HashKey128Type T>
HashKey128<T> HashKey128<T>::mac(const void* data, size_t len, const EncryptionKey* key) {
    HashKey128<T> result;
    if (key == nullptr) {
        set_secure_hash_method(false);
        return non_crypto(data, len, key);
    }
    if constexpr (T == HashKey128Type::HMAC128) {
        std::array<uint8_t, 32> buf{};
        key->hmac_256(static_cast<const uint8_t*>(data), len, buf.data());
        std::memcpy(&result, buf.data(), 16);
    } else if constexpr (T == HashKey128Type::CMAC128) {
        std::array<uint8_t, 16> buf{};
        key->cmac_128(static_cast<const uint8_t*>(data), len, buf.data());
        std::memcpy(&result, buf.data(), 16);
    }
    return result;
}
/** ---------------------------------------------------------------------------------------------------------- HashKey256 mac
 */
template<HashKey256Type T>
template<HashKey256Type U, typename>
HashKey256<T> HashKey256<T>::mac(const void* data, size_t len, const EncryptionKey* key) {
    HashKey256<T> result;
    if constexpr (T == HashKey256Type::CMAC256) {
        std::array<uint8_t, 32> buf{};
        key->cmac_256(static_cast<const uint8_t*>(data), len, buf.data());
        std::memcpy(&result, buf.data(), 32);
    }
    return result;
}
} // namespace buffetalligator
