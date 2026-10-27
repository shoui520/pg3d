#ifndef PG3D_ARCHIVE_H
#define PG3D_ARCHIVE_H

#include <stdint.h>
#include <stdio.h>

void archive_init(void);
/* A read-only stream over base.apk or the OBB, or NULL for any other path
 * (or if it can't be opened; plain fopen then reports why). */
FILE *archive_fopen(const char *vita_path);
/* Saves what boot taught the small-read caches; after each load. */
void archive_after_load(void);

/* The first open archive from index `from` on that could hold a zip entry,
 * given the entry's 30-byte local header (sizes and CRC as in the central
 * directory) and where its data starts; -1 if none. Only the local header is
 * checked, so verify what's read. */
int archive_find_entry(const uint8_t *header, uint32_t data_start, int from);
/* Bytes read, or -1 */
int archive_read_at(int archive, void *dst, uint32_t size, uint32_t offset);

#endif
