#ifndef JAR_IMPORT_H
#define JAR_IMPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Prepare a native Abyssal content pack for an owner-supplied DEEP JAR.
 * The returned path is written to out_path when the cache exists or the
 * conversion succeeds.
 */
int jar_import_prepare(const char *jar_path,
                       const char *cache_root,
                       char *out_path,
                       unsigned out_size);

const char *jar_import_error(void);

/* Live progress for the Switch import overlay. */
int jar_import_progress_active(void);
int jar_import_progress_read(char *stage, unsigned stage_size,
                             char *detail, unsigned detail_size,
                             unsigned *percent, unsigned *done,
                             unsigned *total);

#ifdef __cplusplus
}
#endif

#endif
