#ifndef MD5_H
#define MD5_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Progress callback for md5_file: `done` bytes hashed so far out of `total`
 * (the file size, or 0 if it couldn't be determined). Called once per read
 * chunk. Must not block. */
typedef void (*md5_progress_cb)(void *ud, uint64_t done, uint64_t total);

/* Compute the lowercase hex MD5 digest of a file. `out_hex` must hold at least
 * 33 bytes (32 hex chars + NUL). If `cancel` is non-NULL and becomes true mid-
 * hash, the function aborts and returns false. Returns false if the file can't
 * be read — including a read error partway through, which must never be allowed
 * to surface as a digest of the partial data. A true return means `out_hex` is
 * the digest of the whole file and nothing less. `progress` may be NULL; when
 * set it is invoked with `ud` after each chunk so the UI can show a live bar. */
bool md5_file(const char *path, char out_hex[33], volatile bool *cancel,
              md5_progress_cb progress, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* MD5_H */
