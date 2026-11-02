#include <kernel/fat16.h>
#include <kernel/heap.h>
#include <kernel/vfs.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct vfs_backend {
    void (*list)(void);
    void (*chdir)(const char* path);
    void (*mkdir)(const char* path);
    void (*touch)(const char* path);
    void (*cat)(const char* path);
    void (*write_text)(const char* path, const char* data);
    int (*read_file)(const char* path, void** contents, size_t* size);
    int (*write_file)(const char* path, const void* contents, size_t size);
    int (*unlink)(const char* path);
    int (*rmdir)(const char* path);
} vfs_backend_t;

typedef struct vfs_open_path {
    struct vfs_open_path* next;
    uint32_t references;
    char path[];
} vfs_open_path_t;

static const vfs_backend_t fat16_backend = {
    .list = fat16_ls,
    .chdir = fat16_cd,
    .mkdir = fat16_mkdir,
    .touch = fat16_touch,
    .cat = fat16_cat,
    .write_text = fat16_write,
    .read_file = fat16_load_file,
    .write_file = fat16_write_file,
    .unlink = fat16_unlink,
    .rmdir = fat16_rmdir
};

static const vfs_backend_t* root_backend;
static vfs_open_path_t* open_paths;

static uint32_t lock_vfs(void) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void unlock_vfs(uint32_t flags) {
    if (flags & 0x200) {
        asm volatile("sti" : : : "memory");
    }
}

void vfs_init(void) {
    root_backend = fat16_is_mounted() ? &fat16_backend : 0;
    open_paths = 0;
}

int vfs_is_mounted(void) {
    return root_backend != 0;
}

int vfs_resolve_path(const char* path, char* resolved, size_t capacity) {
    if (!root_backend || !path || !*path || !resolved || capacity < 2 ||
        capacity > VFS_PATH_MAX + 1) {
        return 0;
    }

    size_t length = 1;
    resolved[0] = '/';
    resolved[1] = '\0';

    if (path[0] != '/') {
        const char* cwd = fat16_get_cwd();
        if (!cwd || cwd[0] != '/') {
            return 0;
        }
        while (*cwd) {
            if (*cwd != '/') {
                if (length > 1) {
                    if (length + 1 >= capacity) {
                        return 0;
                    }
                    resolved[length++] = '/';
                }
                while (*cwd && *cwd != '/') {
                    if (length + 1 >= capacity) {
                        return 0;
                    }
                    resolved[length++] = *cwd++;
                }
                resolved[length] = '\0';
            } else {
                cwd++;
            }
        }
    }

    const char* component = path;
    while (*component) {
        while (*component == '/') {
            component++;
        }
        if (!*component) {
            break;
        }

        const char* end = component;
        while (*end && *end != '/') {
            end++;
        }
        size_t component_length = (size_t)(end - component);
        if (component_length == 1 && component[0] == '.') {
            component = end;
            continue;
        }
        if (component_length == 2 && component[0] == '.' &&
            component[1] == '.') {
            if (length > 1) {
                while (length > 1 && resolved[length - 1] != '/') {
                    length--;
                }
                if (length > 1) {
                    length--;
                }
                resolved[length] = '\0';
            }
            component = end;
            continue;
        }

        size_t separator = length > 1 ? 1 : 0;
        if (component_length > VFS_PATH_MAX ||
            length + separator + component_length >= capacity ||
            length + separator + component_length > VFS_PATH_MAX) {
            return 0;
        }
        if (separator) {
            resolved[length++] = '/';
        }
        memcpy(resolved + length, component, component_length);
        length += component_length;
        resolved[length] = '\0';
        component = end;
    }
    return 1;
}

const char* vfs_getcwd(void) {
    return root_backend ? fat16_get_cwd() : "/";
}

void vfs_list(void) {
    if (root_backend) {
        root_backend->list();
    }
}

void vfs_chdir(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (root_backend && vfs_resolve_path(path, resolved, sizeof(resolved))) {
        root_backend->chdir(resolved);
    }
}

void vfs_mkdir(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (root_backend && vfs_resolve_path(path, resolved, sizeof(resolved))) {
        root_backend->mkdir(resolved);
    }
}

void vfs_touch(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (root_backend && vfs_resolve_path(path, resolved, sizeof(resolved))) {
        root_backend->touch(resolved);
    }
}

void vfs_cat(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (root_backend && vfs_resolve_path(path, resolved, sizeof(resolved))) {
        root_backend->cat(resolved);
    }
}

void vfs_write_text(const char* path, const char* data) {
    char resolved[VFS_PATH_MAX + 1];
    if (root_backend && vfs_resolve_path(path, resolved, sizeof(resolved))) {
        root_backend->write_text(resolved, data);
    }
}

int vfs_read_file(const char* path, void** contents, size_t* size) {
    char resolved[VFS_PATH_MAX + 1];
    return root_backend &&
        vfs_resolve_path(path, resolved, sizeof(resolved)) &&
        root_backend->read_file(resolved, contents, size);
}

int vfs_write_file(const char* path, const void* contents, size_t size) {
    char resolved[VFS_PATH_MAX + 1];
    return root_backend &&
        vfs_resolve_path(path, resolved, sizeof(resolved)) &&
        root_backend->write_file(resolved, contents, size);
}

static vfs_open_path_t* find_open_path(const char* path) {
    for (vfs_open_path_t* current = open_paths; current;
         current = current->next) {
        const char* left = current->path;
        const char* right = path;
        while (*left && *right) {
            char left_character = *left++;
            char right_character = *right++;
            if (left_character >= 'A' && left_character <= 'Z') {
                left_character += 'a' - 'A';
            }
            if (right_character >= 'A' && right_character <= 'Z') {
                right_character += 'a' - 'A';
            }
            if (left_character != right_character) {
                break;
            }
        }
        if (!*left && !*right) {
            return current;
        }
    }
    return 0;
}

int vfs_track_open(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (!root_backend ||
        !vfs_resolve_path(path, resolved, sizeof(resolved))) {
        return 0;
    }

    uint32_t flags = lock_vfs();
    vfs_open_path_t* current = find_open_path(resolved);
    if (current) {
        if (current->references == UINT32_MAX) {
            unlock_vfs(flags);
            return 0;
        }
        current->references++;
        unlock_vfs(flags);
        return 1;
    }

    size_t path_length = strlen(resolved);
    current = (vfs_open_path_t*)kmalloc(sizeof(*current) + path_length + 1);
    if (!current) {
        unlock_vfs(flags);
        return 0;
    }
    current->references = 1;
    memcpy(current->path, resolved, path_length + 1);
    current->next = open_paths;
    open_paths = current;
    unlock_vfs(flags);
    return 1;
}

void vfs_track_close(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (!path || !vfs_resolve_path(path, resolved, sizeof(resolved))) {
        return;
    }

    uint32_t flags = lock_vfs();
    vfs_open_path_t* current = find_open_path(resolved);
    if (current && --current->references == 0) {
        vfs_open_path_t** link = &open_paths;
        while (*link != current) {
            link = &(*link)->next;
        }
        *link = current->next;
        kfree(current);
    }
    unlock_vfs(flags);
}

int vfs_unlink(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (!root_backend ||
        !vfs_resolve_path(path, resolved, sizeof(resolved))) {
        return 0;
    }

    uint32_t flags = lock_vfs();
    int is_open = find_open_path(resolved) != 0;
    unlock_vfs(flags);
    return !is_open && root_backend->unlink(resolved);
}

int vfs_rmdir(const char* path) {
    char resolved[VFS_PATH_MAX + 1];
    if (!root_backend ||
        !vfs_resolve_path(path, resolved, sizeof(resolved))) {
        return 0;
    }
    return root_backend->rmdir(resolved);
}
