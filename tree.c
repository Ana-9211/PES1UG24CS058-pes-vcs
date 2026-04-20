// tree.c — Tree object serialization and construction
#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// File mode constants (octal): match POSIX permission bits
// 0100644 = regular file, not executable
// 0100755 = regular file, executable (owner has +x)
// 0040000 = directory (tree object)
#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// Forward decl
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ───────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: given index entries with a given prefix, build tree
// prefix = e.g. "" for root, "src/" for src subdir
static int write_tree_level(IndexEntry *entries, int count, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    int i = 0;
    while (i < count) {
        const char *rel = entries[i].path + strlen(prefix);
        char *slash = strchr(rel, '/');

        if (!slash) {
            // File entry
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name)-1] = '\0';
            i++;
        } else {
            // Directory: collect all entries in this subdir
            char dirname[256];
            size_t dlen = slash - rel;
            strncpy(dirname, rel, dlen);
            dirname[dlen] = '\0';

            // Build new prefix
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dirname);

            // Count entries belonging to this subdir
            int start = i;
            while (i < count && strncmp(entries[i].path, new_prefix, strlen(new_prefix)) == 0) i++;

            // Recurse
            ObjectID subtree_id;
            if (write_tree_level(entries + start, i - start, new_prefix, &subtree_id) != 0)
                return -1;

            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR;
            te->hash = subtree_id;
            strncpy(te->name, dirname, sizeof(te->name) - 1);
            te->name[sizeof(te->name)-1] = '\0';
        }
    }

    // Serialize and write tree object
    void *data;
    size_t data_len;
    if (tree_serialize(&tree, &data, &data_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, data, data_len, id_out);
    free(data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0) return -1;
    if (index.count == 0) {
        // Empty tree
        Tree empty; empty.count = 0;
        void *data; size_t len;
        tree_serialize(&empty, &data, &len);
        int rc = object_write(OBJ_TREE, data, len, id_out);
        free(data);
        return rc;
    }
    return write_tree_level(index.entries, index.count, "", id_out);
}
