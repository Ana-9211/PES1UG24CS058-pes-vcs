// object.c — Content-addressable object store
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// Windows/MSYS2 compatibility
#ifndef O_BINARY
  #define O_BINARY 0
#endif

#ifdef _WIN32
  #include <direct.h>
  #define mkdir_compat(path) _mkdir(path)
  #define fsync(fd) _commit(fd)
#else
  #define mkdir_compat(path) mkdir(path, 0755)
#endif

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header
    const char *type_str = (type == OBJ_BLOB) ? "blob" : (type == OBJ_TREE) ? "tree" : "commit";
    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    size_t full_len = hlen + 1 + len;
    uint8_t *full = malloc(full_len);
    if (!full) return -1;
    memcpy(full, header, hlen);
    full[hlen] = '\0';
    memcpy(full + hlen + 1, data, len);

    // 2. Compute hash
    ObjectID id;
    compute_hash(full, full_len, &id);

    // 3. Deduplication
    if (object_exists(&id)) {
        *id_out = id;
        free(full);
        return 0;
    }

    // 4. Create shard directory
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);
    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir_compat(shard_dir);

    // 5. Write to temp file
    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));
    char tmp_path[520];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", final_path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) { free(full); return -1; }
    write(fd, full, full_len);
    free(full);

    // 6. fsync
    fsync(fd);
    close(fd);

    // 7. rename (atomic)
    // On Windows rename fails if destination exists, so remove first
    remove(final_path);
    if (rename(tmp_path, final_path) != 0) return -1;

    // 8. Store hash
    *id_out = id;
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(file_size);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, file_size, f);
    fclose(f);

    // Verify integrity
    ObjectID computed;
    compute_hash(buf, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    // Parse header: find '\0'
    uint8_t *null_pos = memchr(buf, '\0', file_size);
    if (!null_pos) { free(buf); return -1; }

    // Parse type
    if (strncmp((char*)buf, "blob ", 5) == 0) *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree ", 5) == 0) *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Parse size
    char *space_pos = (char*)memchr(buf, ' ', null_pos - buf);
    if (!space_pos) { free(buf); return -1; }
    size_t data_size;
    sscanf(space_pos + 1, "%zu", &data_size);

    // Copy data portion
    uint8_t *data_start = null_pos + 1;
    *len_out = data_size;
    *data_out = malloc(data_size + 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, data_start, data_size);
    ((char*)*data_out)[data_size] = '\0';

    free(buf);
    return 0;
}
