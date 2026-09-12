/** --------------------------------------------------------------------------------------------------------- Wire Encoding
 * @file ba_wire.c
 * @brief Encodes portable placement names and authenticates optional XChaCha20-Poly1305 frames.
 */
#include "ba_network.h"
#include <stdlib.h>
#include <string.h>

/** --------------------------------------------------------------------------------------------------------- Read Integer
 * @brief Reads one big-endian unsigned integer.
 */
static uint64_t ba_wire_read(const unsigned char* source, size_t bytes) {
    uint64_t value = 0;
    for (size_t index = 0; index < bytes; ++index) value = (value << 8) | source[index];
    return value;
}
/** --------------------------------------------------------------------------------------------------------- Write Integer
 * @brief Writes one big-endian unsigned integer.
 */
static void ba_wire_write(unsigned char* target, uint64_t value, size_t bytes) {
    for (size_t index = bytes; index; --index) {
        target[index - 1] = (unsigned char)value;
        value >>= 8;
    }
}
/** --------------------------------------------------------------------------------------------------------- Key
 * @brief Loads an explicitly configured shared key without printing its contents.
 */
int ba_net_key(unsigned char key[32]) {
    const char* encoded = getenv("ALLIGATOR_NETWORK_KEY");
    size_t bytes = 0;
    if (!encoded || strlen(encoded) != 64) return UV_EACCES;
    if (sodium_hex2bin(key, 32, encoded, 64, NULL, &bytes, NULL) || bytes != 32) {
        sodium_memzero(key, 32);
        return UV_EACCES;
    }
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Frame Size
 * @brief Validates the fixed header before accepting its advertised allocation size.
 */
int ba_net_frame_size(const unsigned char* header, size_t* size) {
    if (memcmp(header, "BAGN", 4) || header[4] != 1 || (header[5] & ~15u)) return UV_EPROTO;
    if ((header[5] & BA_NET_RESPONSE) && (header[5] & BA_NET_REPLY)) return UV_EPROTO;
    for (size_t index = 56; index < BA_NET_HEADER; ++index)
        if (header[index]) return UV_EPROTO;
    const uint64_t names = ba_wire_read(header + 6, 2);
    const uint64_t bytes = ba_wire_read(header + 8, 8);
    if (!names || names > 255 || !bytes || bytes > BA_NET_LIMIT ||
        bytes > (UINT64_MAX >> BA_SLOT_BITS))
        return UV_EMSGSIZE;
    *size = BA_NET_HEADER + (size_t)names + (size_t)bytes +
            ((header[5] & BA_NET_SECURE) ? crypto_aead_xchacha20poly1305_ietf_ABYTES : 0);
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Encode
 * @brief Copies only the Slice view and its placement name into an authenticated frame.
 */
int ba_net_encode(const ba_slice_t* slice, unsigned flags, const unsigned char token[16],
                  const unsigned char key[32], ba_net_frame* frame) {
    if (slice->meta == BA_NULL_META) return UV_EINVAL;
    const size_t bytes = (size_t)(slice->meta >> BA_SLOT_BITS);
    const char* name = ba_placement_name(ba_slice_placement(slice));
    const size_t names = strlen(name);
    if (!bytes || bytes > BA_NET_LIMIT || !names || names > 255) return UV_EMSGSIZE;
    const size_t plain_size = names + bytes;
    frame->size = BA_NET_HEADER + plain_size + ((flags & BA_NET_SECURE) ? 16 : 0);
    frame->data = calloc(1, frame->size);
    if (!frame->data) return UV_ENOMEM;
    unsigned char* header = frame->data;
    memcpy(header, "BAGN", 4);
    header[4] = 1;
    header[5] = (unsigned char)(flags | (ba_slice_is_novel(slice) ? BA_NET_NOVEL : 0));
    ba_wire_write(header + 6, names, 2);
    ba_wire_write(header + 8, bytes, 8);
    memcpy(header + 16, token, 16);
    unsigned char* payload = header + BA_NET_HEADER;
    memcpy(payload, name, names);
    memcpy(payload + names, slice->ptr, bytes);
    if (flags & BA_NET_SECURE) {
        randombytes_buf(header + 32, 24);
        crypto_aead_xchacha20poly1305_ietf_encrypt(payload, NULL, payload, plain_size, header,
                                                   BA_NET_HEADER, NULL, header + 32, key);
    }
    return 0;
}
/** --------------------------------------------------------------------------------------------------------- Decode
 * @brief Authenticates the entire frame before resolving placement and allocating received memory.
 */
int ba_net_decode(const unsigned char* frame, size_t size, uint8_t protocol,
                  const unsigned char key[32], ba_slice_t* slice) {
    *slice = (ba_slice_t){BA_NULL_META, NULL};
    if (size < BA_NET_HEADER) return UV_EPROTO;
    size_t expected;
    int status = ba_net_frame_size(frame, &expected);
    if (status || expected != size) return status ? status : UV_EPROTO;
    if (!!(frame[5] & BA_NET_SECURE) != !!(protocol & 128)) return UV_EACCES;
    const size_t names = (size_t)ba_wire_read(frame + 6, 2);
    const size_t bytes = (size_t)ba_wire_read(frame + 8, 8);
    const unsigned char* payload = frame + BA_NET_HEADER;
    unsigned char* decrypted = NULL;
    if (protocol & 128) {
        decrypted = malloc(names + bytes);
        if (!decrypted) return UV_ENOMEM;
        if (crypto_aead_xchacha20poly1305_ietf_decrypt(decrypted, NULL, NULL, payload,
                                                       size - BA_NET_HEADER, frame, BA_NET_HEADER,
                                                       frame + 32, key)) {
            free(decrypted);
            return UV_EACCES;
        }
        payload = decrypted;
    }
    char name[256];
    memcpy(name, payload, names);
    name[names] = 0;
    const int placement = memchr(name, 0, names) ? -1 : ba_net_placement(name);
    if (placement < 0) status = UV_ENOENT;
    else if (ba_claim((uint32_t)placement, bytes, (frame[5] & BA_NET_NOVEL) ? BA_CLAIM_NOVEL : 0,
                      slice) != 0)
        status = UV_ENOMEM;
    else {
        memcpy(slice->ptr, payload + names, bytes);
    }
    if (decrypted) {
        sodium_memzero(decrypted, names + bytes);
        free(decrypted);
    }
    return status;
}
