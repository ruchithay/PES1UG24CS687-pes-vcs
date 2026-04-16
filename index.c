// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6... 1699900000 42 README.md
//   100644 f7e8d9c0b1a2... 1699900100 128 src/main.c
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions: index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declarations from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged: %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted: %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("    modified: %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Comparator for sorting index entries by path
static int compare_index_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

// Load the index from .pes/index.
// If the file does not exist, initializes an empty index (not an error).
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) {
        // File doesn't exist yet — empty index is fine
        return 0;
    }

    char hex[HASH_HEX_SIZE + 2]; // +2 for safety
    unsigned int mode;
    unsigned long long mtime;
    unsigned int size;
    char path[512];

    while (index->count < MAX_INDEX_ENTRIES) {
        // Format: <mode-octal> <hex-hash> <mtime> <size> <path>
        int n = fscanf(f, "%o %64s %llu %u %511s\n",
                       &mode, hex, &mtime, &size, path);
        if (n == EOF) break;
        if (n != 5) {
            fclose(f);
            return -1;
        }

        IndexEntry *e = &index->entries[index->count];
        e->mode = (uint32_t)mode;
        if (hex_to_hash(hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }
        e->mtime_sec = (uint64_t)mtime;
        e->size = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    // Sort entries by path before writing
    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES)
    return -1;
    Index sorted;
sorted.count = index->count;

memcpy(sorted.entries, index->entries,
       sizeof(IndexEntry) * index->count);

qsort(sorted.entries, sorted.count,
      sizeof(IndexEntry), compare_index_by_path);
    // Write to a temp file
    char tmp_path[] = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "w");
	if (!f) {
	    perror("fopen");
	    return -1;
	}
    char hex[HASH_HEX_SIZE + 1];
    if (index->count == 0) {
    printf("Nothing to save\n");
}
    for (int i = 0; i < sorted.count; i++) {
        const IndexEntry *e = &sorted.entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n",
                (unsigned int)e->mode,
                hex,
                (unsigned long long)e->mtime_sec,
                (unsigned int)e->size,
                e->path);
    }

    // Flush userspace buffers and sync to disk
    fflush(f);
    int fd = fileno(f);
if (fd >= 0) fsync(fd);
    fclose(f);

    // Atomically replace the index file
    if (rename(tmp_path, INDEX_FILE) != 0) {
        unlink(tmp_path);
        return -1;
    }

    return 0;
}

// Stage a file for the next commit.
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // 1. Open and read the file
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    void *contents = malloc((size_t)file_size);
    if (!contents) {
        fclose(f);
        return -1;
    }

    if (fread(contents, 1, (size_t)file_size, f) != (size_t)file_size) {
        fclose(f);
        free(contents);
        return -1;
    }
    fclose(f);

    // 2. Write the contents as a blob
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents, (size_t)file_size, &blob_id) != 0) {
        free(contents);
        return -1;
    }
    free(contents);

    // 3. Get file metadata (mode, mtime, size)
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    // 4. Find or create the index entry
    IndexEntry *existing = index_find(index, path);
    IndexEntry *entry;

    if (existing) {
        entry = existing;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        entry = &index->entries[index->count++];
        memset(entry, 0, sizeof(IndexEntry));
        strncpy(entry->path, path, sizeof(entry->path) - 1);
        entry->path[sizeof(entry->path) - 1] = '\0';
    }

    entry->hash = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;

    // Determine mode: executable or regular
    if (st.st_mode & S_IXUSR)
        entry->mode = 0100755;
    else
        entry->mode = 0100644;

    // 5. Save the index
    return index_save(index);
}
