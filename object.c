// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions: object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

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
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
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

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
// where <type> is "blob", "tree", or "commit"
// and <size> is the decimal string of the data length
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Choose the type string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // 2. Build the full object: header + '\0' + data
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    // header_len does not include the '\0' terminator we need
    size_t full_len = (size_t)header_len + 1 + len; // +1 for the null byte
    uint8_t *full_obj = malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, (size_t)header_len);
    full_obj[header_len] = '\0';
    memcpy(full_obj + header_len + 1, data, len);

    // 3. Compute SHA-256 hash of the FULL object
    ObjectID id;
    compute_hash(full_obj, full_len, &id);

    // 4. Check for deduplication — if already stored, nothing to do
    if (object_exists(&id)) {
        free(full_obj);
        *id_out = id;
        return 0;
    }

    // 5. Build the shard directory path and create it if needed
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    char shard_dir[256];
    snprintf(shard_dir, sizeof(shard_dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(shard_dir, 0755); // Ignore error if already exists

    // 6. Build final path and a temp path in the same directory
    char final_path[512];
    snprintf(final_path, sizeof(final_path), "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/%.2s/%s.tmp", OBJECTS_DIR, hex, hex + 2);

    // 7. Write to the temp file
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    ssize_t written = write(fd, full_obj, full_len);
    free(full_obj);

    if (written < 0 || (size_t)written != full_len) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // 8. fsync the temp file to ensure data reaches disk
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    // 9. Atomically rename temp file to final path
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    // 10. fsync the shard directory to persist the rename
    int dir_fd = open(shard_dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    // 11. Return the computed hash
    *id_out = id;
    return 0;
}

// Read an object from the store.
//
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
// The caller is responsible for calling free(*data_out).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Build the file path from the hash
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *buf = malloc((size_t)file_size);
    if (!buf) {
        fclose(f);
        return -1;
    }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    // 3. Verify integrity: recompute SHA-256 and compare to expected hash
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(buf);
        return -1;
    }

    // 4. Find the '\0' separator between header and data
    uint8_t *null_pos = memchr(buf, '\0', (size_t)file_size);
    if (!null_pos) {
        free(buf);
        return -1;
    }

    // 5. Parse the type string from the header
    if (strncmp((char *)buf, "blob ", 5) == 0) {
        *type_out = OBJ_BLOB;
    } else if (strncmp((char *)buf, "tree ", 5) == 0) {
        *type_out = OBJ_TREE;
    } else if (strncmp((char *)buf, "commit ", 7) == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        free(buf);
        return -1;
    }

    // 6. Extract the data portion (everything after the '\0')
    uint8_t *data_start = null_pos + 1;
    size_t data_len = (size_t)file_size - (size_t)(data_start - buf);

    uint8_t *data_copy = malloc(data_len + 1); // +1 for safety null terminator
    if (!data_copy) {
        free(buf);
        return -1;
    }

    memcpy(data_copy, data_start, data_len);
    data_copy[data_len] = '\0'; // null-terminate for convenience (not counted in len)

    free(buf);

    *data_out = data_copy;
    *len_out = data_len;
    return 0;
}
