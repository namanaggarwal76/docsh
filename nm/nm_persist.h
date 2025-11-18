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

// Query whether a user is currently marked active (1) or not (0)
int nm_state_user_is_active(const char *user);

// Set a user's active status; if active=1 ensures user exists in users list. Returns 1 on change, 0 if unchanged
int nm_state_set_user_active(const char *user, int active);

// Snapshot users into caller-provided buffer of fixed-size strings
// Returns number of users copied (<= max_users)
size_t nm_state_get_users(char users[][128], size_t max_users);

// Snapshot active (logged-in) users into caller-provided buffer of fixed-size strings
// Returns number of active users copied (<= max_users)
size_t nm_state_get_active_users(char users[][128], size_t max_users);

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

// --- Metadata tracking (last modified/accessed user and time) ---
// Set last modified user and time for a file; returns 1 on success, 0 if file not found
int nm_state_set_file_modified(const char *file, const char *user, int time);

// Set last accessed user and time for a file; returns 1 on success, 0 if file not found
int nm_state_set_file_accessed(const char *file, const char *user, int time);

// Get file metadata (last modified/accessed user and time); returns 0 on success, -1 if file not found
int nm_state_get_file_metadata(const char *file, char *mod_user_out, size_t mod_user_sz, int *mod_time_out, char *acc_user_out, size_t acc_user_sz, int *acc_time_out);

// --- ACLs (M7) ---
// Permissions bitmask
#define ACL_R 1
#define ACL_W 2

/* ACL MODEL (Unified)
 * --------------------
 * Permissions are stored per FILE, not per USER.
 * Each file has:
 *   - owner: single username with implicit RW
 *   - grants[]: dynamic array of (user, perm) where perm bitmask uses ACL_R (1) | ACL_W (2)
 * Anonymous/public access is represented by a grant with user="anonymous" (perm may include R and/or W).
 * All authorization checks call nm_acl_check(file, user, op), which:
 *   - Allows owner automatically
 *   - Maps op to required perm (READ-like => R; WRITE/UNDO/REVERT/etc => W; W implies R when granted)
 *   - Searches grants for exact user or falls back to the anonymous grant
 * There is intentionally NO global user-centric permission index; all lookups traverse the file's grants.
 * This file-centric design simplifies rename/move operations and persistence.
 */

// Set or update owner for a file (owner always has RW)
int nm_acl_set_owner(const char *file, const char *owner);

// Grant permissions to a user for a file (R/W bits). Upsert behavior.
int nm_acl_grant(const char *file, const char *user, int perm);

// Remove all permissions for a user on a file
int nm_acl_revoke(const char *file, const char *user);

// Remove ACL entry entirely for a file (owner and all grants)
int nm_acl_delete(const char *file);

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
// Add a pending access request for file by username with mode ('R' or 'W');
// returns 1 if added, 0 if already present or file unknown
int nm_state_add_request(const char *file, const char *user, char mode);

// List pending requests for file into users array and modes array; returns count
size_t nm_state_list_requests(const char *file, char users[][128], char modes[], size_t max_users);

// Remove a pending request for file by username; returns 1 if removed, 0 if not present
int nm_state_remove_request(const char *file, const char *user);

// Remove all pending requests for a file
int nm_state_clear_requests_for(const char *file);

// --- Trash (soft delete) ---
// Add a trashed entry with original file, trashed storage path, ssid, owner and time (epoch seconds)
int nm_state_trash_add(const char *file, const char *trashed_path, int ssid, const char *owner, int when);

// Remove a trashed entry by original file name; returns 1 if removed
int nm_state_trash_remove(const char *file);

// Find a trashed entry by original file; returns 0 on success and fills outputs
int nm_state_trash_find(const char *file, char *trashed_out, size_t trashed_out_sz, int *ssid_out, char *owner_out, size_t owner_out_sz, int *when_out);

// Snapshot trash into caller-provided fixed arrays; returns number of entries
size_t nm_state_get_trash(char files[][128], char trashed[][128], int ssids[], char owners[][128], int whens[], size_t max_entries);

#endif // NM_PERSIST_H