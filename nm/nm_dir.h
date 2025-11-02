#ifndef NM_DIR_H
#define NM_DIR_H

#include <stddef.h>

// Initialize directory and LRU from persisted state
void nm_dir_init(void);

// Lookup a file; returns 0 and sets *out_ss_id on success, -1 if not found
int nm_dir_lookup(const char *file, int *out_ss_id);

// Upsert mapping and update persistence; returns 1 if added/changed
int nm_dir_set(const char *file, int ss_id);

// Build a JSON array of files (with ss_id) into dst; returns bytes written
size_t nm_dir_build_view_json(char *dst, size_t dst_sz, int include_ss);

// Delete mapping; returns 1 if removed
int nm_dir_del(const char *file);

// Rename mapping and update persistence and LRU; returns 1 if renamed
int nm_dir_rename(const char *old_file, const char *new_file);

#endif // NM_DIR_H