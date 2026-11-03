#ifndef _KERNEL_VFS_H
#define _KERNEL_VFS_H

#include <stddef.h>

#define VFS_PATH_MAX 255

void vfs_init(void);
int vfs_is_mounted(void);
int vfs_format_fat16(void);
int vfs_resolve_path(const char* path, char* resolved, size_t capacity);
const char* vfs_getcwd(void);
void vfs_list(void);
void vfs_chdir(const char* path);
void vfs_mkdir(const char* path);
void vfs_touch(const char* path);
void vfs_cat(const char* path);
void vfs_write_text(const char* path, const char* data);
int vfs_read_file(const char* path, void** contents, size_t* size);
int vfs_write_file(const char* path, const void* contents, size_t size);
int vfs_unlink(const char* path);
int vfs_rmdir(const char* path);
int vfs_track_open(const char* path);
void vfs_track_close(const char* path);

#endif
