#ifndef NM_PERSIST_H
#define NM_PERSIST_H

#include <stddef.h>

// Initialize in-memory NM state
void nm_state_init(void);

// Load state from JSON file; returns 0 on success, -1 on error (non-fatal)
int nm_state_load(const char *path);

// Save state to JSON file atomically (write+rename); returns 0 on success
int nm_state_save(const char *path);

// Add a user to active sessions set; returns 1 if added, 0 if already existed
int nm_state_add_user(const char *user);

// Snapshot users into caller-provided buffer of fixed-size strings
// Returns number of users copied (<= max_users)
size_t nm_state_get_users(char users[][128], size_t max_users);

// Directory mapping persistence (file -> ssId)
// Upsert a mapping; returns 1 if added or changed, 0 if unchanged
int nm_state_set_dir(const char *file, int ss_id);

// Find mapping; returns 0 on success, -1 if not found
int nm_state_find_dir(const char *file, int *out_ss_id);

// Snapshot directory into arrays; returns number of entries copied
size_t nm_state_get_dir(char files[][128], int ss_ids[], size_t max_entries);

// Remove mapping; returns 1 if removed, 0 if not found
int nm_state_del_dir(const char *file);

// Rename mapping key; returns 1 if renamed, 0 if src not found or dst exists
int nm_state_rename_dir(const char *old_file, const char *new_file);

// --- Replication metadata (bonus) ---
// Set full replica list for a file (array of ssIds). Returns 1 on change, 0 if unchanged, -1 on error.
int nm_state_set_replicas(const char *file, const int *replicas, size_t n);

// Copy up to max replica ssIds into out; returns number of replicas. 0 if none.
size_t nm_state_get_replicas(const char *file, int *out, size_t max);

// Convenience: get primary ssId for a file (wraps directory mapping). Returns 0 on success, -1 if not found.
int nm_state_get_primary(const char *file, int *out_ssid);

// --- ACLs (M7) ---
// Permissions bitmask
#define ACL_R 1
#define ACL_W 2

// Set or update owner for a file (owner always has RW)
int nm_acl_set_owner(const char *file, const char *owner);

// Grant permissions to a user for a file (R/W bits). Upsert behavior.
int nm_acl_grant(const char *file, const char *user, int perm);

// Remove all permissions for a user on a file
int nm_acl_revoke(const char *file, const char *user);

// Check if user is allowed for op ("READ" requires R, "WRITE"/"UNDO" require W). Owner is always allowed.
int nm_acl_check(const char *file, const char *user, const char *op);

// Rename ACL entry for a file to a new name (owner/grants preserved); returns 1 if renamed
int nm_acl_rename(const char *old_file, const char *new_file);

// Get owner into provided buffer; returns 0 on success, -1 if not found
int nm_acl_get_owner(const char *file, char *owner_out, size_t owner_out_sz);

// Format access list as "user1 (RW), user2 (R)" into dst; returns bytes written, or 0 if none
size_t nm_acl_format_access(const char *file, char *dst, size_t dst_sz);

// --- Folders (M13, bonus) ---
// Add a folder path (e.g., "docs/reports"); returns 1 if added, 0 if already existed
int nm_state_add_folder(const char *path);

// Remove a folder path; returns 1 if removed
int nm_state_remove_folder(const char *path);

// Snapshot folders into caller buffer; returns number of entries copied
size_t nm_state_get_folders(char folders[][256], size_t max_entries);

// Rename/move a folder prefix old_path -> new_path in folder list and directory mappings
// Returns number of files remapped (>=0). Does not contact SS (caller must orchestrate renames).
int nm_state_move_folder_prefix(const char *old_path, const char *new_path,
								char files[][128], char new_files[][128], int ssids[], size_t max_files);

// --- Access Requests (bonus) ---
// Add a pending access request for file by username; returns 1 if added, 0 if already present or file unknown
int nm_state_add_request(const char *file, const char *user);

// List pending requests for file into users array; returns count
size_t nm_state_list_requests(const char *file, char users[][128], size_t max_users);

// Remove a pending request for file by username; returns 1 if removed, 0 if not present
int nm_state_remove_request(const char *file, const char *user);

#endif // NM_PERSIST_H