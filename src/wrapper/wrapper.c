/************************************************************\
 * Copyright 2021 Lawrence Livermore National Security, LLC
 * (c.f. AUTHORS, NOTICE.LLNS, COPYING)
 *
 * This file is part of the Flux resource manager framework.
 * For details, see https://github.com/flux-framework.
 *
 * SPDX-License-Identifier: LGPL-3.0
\************************************************************/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif // _GNU_SOURCE

#if defined(__cplusplus)
#include <cstdio>
#include <cstdlib>
#include <climits>
#include <cstring>
#include <cerrno>
#include <cstdarg>
#include <ctime>
using namespace std; // std::clock ()
//#include <cstdbool> // c++11
#else
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <stdbool.h>
#endif // defined(__cplusplus)

#include <fcntl.h>
#include <libgen.h> // dirname
#include <dlfcn.h>
#include <unistd.h>
#include <flux/core.h>
#include "dyad.h"
#include "dyad_ctx.h"
#include "utils.h"
#include "murmur3.h"
#include "wrapper.h"

// Debug message
#ifndef DPRINTF
#define DPRINTF(fmt,...) do { \
    if (ctx && ctx->debug) fprintf (stderr, fmt, ##__VA_ARGS__); \
} while (0)
#endif // DPRINTF

#define TIME_DIFF(Tstart, Tend ) \
    ((double) (1000000000L * ((Tend).tv_sec  - (Tstart).tv_sec) + \
                              (Tend).tv_nsec - (Tstart).tv_nsec) / 1000000000L)

// Detailed information message that can be omitted
#if DYAD_FULL_DEBUG
#define IPRINTF DPRINTF
#define IPRINTF_DEFINED
#else
#define IPRINTF(fmt,...)
#endif // DYAD_FULL_DEBUG

#if !defined(DYAD_LOGGING_ON) || (DYAD_LOGGING_ON == 0)
#define FLUX_LOG_INFO(...) do {} while (0)
#define FLUX_LOG_ERR(...) do {} while (0)
#else
#define FLUX_LOG_INFO(...) flux_log (ctx->h, LOG_INFO, __VA_ARGS__)
#define FLUX_LOG_ERR(...) flux_log_error (ctx->h, __VA_ARGS__)
#endif

__thread dyad_sync_ctx_t *ctx = NULL;
static void dyad_sync_init (void) __attribute__((constructor));
static void dyad_sync_fini (void) __attribute__((destructor));

int open_real (const char *path, int oflag, ...);
FILE *fopen_real (const char *path, const char *mode);
int close_real (int fd);
int fclose_real (FILE *fp);

#if DYAD_SYNC_DIR
int sync_directory (const char* path);
#endif // DYAD_SYNC_DIR

/*****************************************************************************
 *                                                                           *
 *                   DYAD Sync Internal API                                  *
 *                                                                           *
 *****************************************************************************/

/**
 * Checks if the file descriptor was opened in write-only mode
 *
 * @param[in] fd The file descriptor to check
 *
 * @return 1 if the file descriptor is write-only, 0 if not, and -1
 *         if there was an error in fcntl
 */
static inline int is_wronly (int fd)
{
    int rc = fcntl (fd, F_GETFL);
    if (rc == -1)
        return -1;
    if ((rc & O_ACCMODE) == O_WRONLY)
        return 1;
    return 0;
}

static inline bool is_dyad_producer ()
{
    char *e = NULL;
    if ((e = getenv (DYAD_KIND_PROD_ENV)))
        return (atoi (e) > 0);
    return false;
}

static inline bool is_dyad_consumer ()
{
    char *e = NULL;
    if ((e = getenv (DYAD_KIND_CONS_ENV)))
        return (atoi (e) > 0);
    return false;
}

static int gen_path_key (const char* str, char* path_key, const size_t len,
    const uint32_t depth, const uint32_t width)
{
    static const uint32_t seeds [10]
        = {104677, 104681, 104683, 104693, 104701,
           104707, 104711, 104717, 104723, 104729};

    uint32_t seed = 57;
    uint32_t hash[4] = {0u}; // Output for the hash
    size_t cx = 0ul;
    int n = 0;

    if (path_key == NULL || len == 0ul) {
        return -1;
    }
    path_key [0] = '\0';

    for (uint32_t d = 0u; d < depth; d++)
    {
        seed += seeds [d % 10];
        MurmurHash3_x64_128 (str, strlen (str), seed, hash);
        uint32_t bin = (hash[0] ^ hash[1] ^ hash[2] ^ hash[3]) % width;
        n = snprintf (path_key+cx, len-cx, "%x.", bin);
        cx += n;
        if (cx >= len || n < 0) {
            return -1;
        }
    }
    n = snprintf (path_key+cx, len-cx, "%s", str);
    if (cx + n >= len || n < 0) {
        return -1;
    }
    return 0;
}

#if DYAD_PERFFLOW
__attribute__((annotate("@critical_path()")))
int dyad_kvs_lookup (const char *topic, uint32_t *owner_rank)
{
    flux_future_t *f1 = NULL;

    // Look up key-value-store for the owner of a file
    f1 = flux_kvs_lookup (ctx->h, ctx->kvs_namespace, FLUX_KVS_WAITCREATE, topic);
    if (f1 == NULL) {
        FLUX_LOG_ERR ("flux_kvs_lookup(%s) failed.\n", topic);
        return -1;
    }

    // Extract the value from the reply
    if (flux_kvs_lookup_get_unpack (f1, "i", owner_rank) < 0) {
        FLUX_LOG_ERR ("flux_kvs_lookup_get_unpack() failed.\n");
        return -1;
    }
    flux_future_destroy (f1);
    f1 = NULL;
    return 0;
}

__attribute__((annotate("@critical_path()")))
int dyad_rpc_pack (flux_future_t **f2, uint32_t owner_rank, const char *user_path)
{
    // Send the request to fetch a file
    if (!(*f2 = flux_rpc_pack (ctx->h, "dyad.fetch", owner_rank, 0,
                              "{s:s}", "upath", user_path))) {
        FLUX_LOG_ERR ("flux_rpc_pack({dyad.fetch %s})", user_path);
        return -1;
    }

    return 0;
}

__attribute__((annotate("@critical_path()")))
int dyad_rpc_get_raw (flux_future_t *f2, const char **file_data,
                      int *file_len, const char *user_path)
{
    // Extract the data from reply
    if (flux_rpc_get_raw (f2, (const void **) file_data, file_len) < 0) {
        FLUX_LOG_ERR ("flux_rpc_get_raw(\"%s\") failed.\n", user_path);
        return -1;
    }
    return 0;
}
#endif // DYAD_PERFFLOW

#if DYAD_PERFFLOW
__attribute__((annotate("@critical_path()")))
#endif
static int subscribe_via_flux (const char *consumer_path, const char *user_path)
{
    int rc = -1;
    uint32_t owner_rank = 0u;
    flux_future_t *f1 = NULL;
    flux_future_t *f2 = NULL;
    const char *file_data = NULL;
    int file_len = 0;
    const char *odir = NULL;
    mode_t m = (S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH | S_ISGID);
    FILE *of = NULL;

    const size_t topic_len = PATH_MAX;
    char topic [topic_len + 1];
    char file_path [PATH_MAX+1] = {'\0'};
    char file_path_copy [PATH_MAX+1] = {'\0'};

    memset (topic, '\0', topic_len + 1);
    gen_path_key (user_path, topic, topic_len, ctx->key_depth, ctx->key_bins);

    if (!ctx->h)
        goto done;

    FLUX_LOG_INFO ("DYAD_SYNC CONS: subscribe_via_flux() for \"%s\".\n", topic);

#if DYAD_PERFFLOW
    dyad_kvs_lookup (topic, &owner_rank);
#else
    // Look up key-value-store for the owner of a file
    f1 = flux_kvs_lookup (ctx->h, ctx->kvs_namespace, FLUX_KVS_WAITCREATE, topic);
    if (f1 == NULL) {
        FLUX_LOG_ERR ("flux_kvs_lookup(%s) failed.\n", topic);
        goto done;
    }

    // Extract the value from the reply
    if (flux_kvs_lookup_get_unpack (f1, "i", &owner_rank) < 0) {
        FLUX_LOG_ERR ("flux_kvs_lookup_get_unpack() failed.\n");
        goto done;
    }
    flux_future_destroy (f1);
    f1 = NULL;
#endif // DYAD_PERFFLOW

    FLUX_LOG_INFO ( \
              "DYAD_SYNC CONS: flux_kvs_lookup(%s) identifies the owner %u\n", \
               topic, owner_rank);

    // If the owner is on the same storage, then no need to transfer the file.
    if (ctx->shared_storage || (owner_rank == ctx->rank)) {
        rc = 0;
        goto done;
    }

#if DYAD_PERFFLOW
   if (dyad_rpc_pack (&f2, owner_rank, user_path))
       goto done;
   if (dyad_rpc_get_raw (f2, &file_data, &file_len, user_path))
       goto done;
#else
    // Send the request to fetch a file
    if (!(f2 = flux_rpc_pack (ctx->h, "dyad.fetch", owner_rank, 0,
                              "{s:s}", "upath", user_path))) {
        FLUX_LOG_ERR ("flux_rpc_pack({dyad.fetch %s})", user_path);
        goto done;
    }

    // Extract the data from reply
    if (flux_rpc_get_raw (f2, (const void **) &file_data, &file_len) < 0) {
        FLUX_LOG_ERR ("flux_rpc_get_raw(\"%s\") failed.\n", user_path);
        goto done;
    }
#endif // DYAD_PERFFLOW

    FLUX_LOG_INFO ("The size of file received: %d.\n", file_len);

    // Set output file path
    strncpy (file_path, consumer_path, PATH_MAX-1);
    concat_str (file_path, user_path, "/", PATH_MAX);
    strncpy (file_path_copy, file_path, PATH_MAX); // dirname modifies the arg

    // Create the directory as needed
    // TODO: Need to be consistent with the mode at the source
    odir = dirname (file_path_copy);
    m = (S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH | S_ISGID);
    if ((strncmp (odir, ".", strlen(".")) != 0) &&
        (mkdir_as_needed (odir, m) < 0)) {
        FLUX_LOG_ERR ("Failed to create directory \"%s\".\n", odir);
        goto done;
    }

    // Write the file.
    of = fopen_real (file_path, "w");
    if (of == NULL) {
        FLUX_LOG_ERR ("Could not open to write file: %s\n", file_path);
        goto done;
    }
    if (fwrite (file_data, sizeof (char), (size_t) file_len, of)
        != (size_t) file_len) {
        FLUX_LOG_ERR ("Could not write file: %s\n", file_path);
        goto done;
    }
    // Close the file
    rc = ((fclose_real (of) == 0)? 0 : -1);

    flux_future_destroy (f2);
    f2 = NULL;

done:
    if (f1 != NULL) {
        flux_future_destroy (f1);
    }
    if (f2 != NULL) {
        flux_future_destroy (f2);
    }
    return rc;
}

#if DYAD_PERFFLOW
__attribute__((annotate("@critical_path()")))
int dyad_kvs_commit (flux_kvs_txn_t *txn, flux_future_t **f,
                  const char *user_path)
{
    if (!(*f = flux_kvs_commit (ctx->h, ctx->kvs_namespace, 0, txn))) {
        FLUX_LOG_ERR ("flux_kvs_commit(owner rank of %s = %u)\n", \
                       user_path, ctx->rank);
        return -1;
    }

    flux_future_wait_for (*f, -1.0);
    flux_future_destroy (*f);
    *f = NULL;
    flux_kvs_txn_destroy (txn);

    return 0;
}
#endif // DYAD_PERFFLOW

#if DYAD_PERFFLOW
__attribute__((annotate("@critical_path()")))
#endif
static int publish_via_flux (const char *producer_path, const char *user_path)
{
    int rc = -1;
    flux_kvs_txn_t *txn = NULL;
    flux_future_t *f = NULL;

    const size_t topic_len = PATH_MAX;
    char topic [topic_len + 1];

    memset (topic, '\0', topic_len + 1);
    gen_path_key (user_path, topic, topic_len, ctx->key_depth, ctx->key_bins);

    if (!ctx->h)
        goto done;

    FLUX_LOG_INFO ("DYAD_SYNC PROD: publish_via_flux() for \"%s\".\n", topic);

    // Register the owner of the file into key-value-store
    if (!(txn = flux_kvs_txn_create ())) {
        FLUX_LOG_ERR ("flux_kvs_txn_create() failed.\n");
        goto done;
    }
    if (flux_kvs_txn_pack (txn, 0, topic, "i", ctx->rank) < 0) {
        FLUX_LOG_ERR ("flux_kvs_txn_pack(\"%s\",\"i\",%u) failed.\n", \
                       topic, ctx->rank);
        goto done;
    }

#if DYAD_PERFFLOW
    if (dyad_kvs_commit (txn, &f, user_path) < 0)
        goto done;
#else

    if (!(f = flux_kvs_commit (ctx->h, ctx->kvs_namespace, 0, txn))) {
        FLUX_LOG_ERR ("flux_kvs_commit(owner rank of %s = %u)\n", \
                       user_path, ctx->rank);
        goto done;
    }

    flux_future_wait_for (f, -1.0);
    flux_future_destroy (f);
    f = NULL;
    flux_kvs_txn_destroy (txn);
#endif // DYAD_PERFFLOW

    rc = 0;

done:
    return rc;
}

static int dyad_open_sync (const char* __restrict__ path,
                           const char* __restrict__ dyad_path,
                           const char* __restrict__ user_path)
{
    int rc = 0;
    ctx->reenter = false;
    rc = subscribe_via_flux (dyad_path, user_path);
    ctx->reenter = true;
    return rc;
}

static int dyad_close_sync (const char* __restrict__ path,
                            const char* __restrict__ dyad_path,
                            const char* __restrict__ user_path)
{
    int rc = 0;
    ctx->reenter = false;
    rc = publish_via_flux (dyad_path, user_path);
    ctx->reenter = true;
    return rc;
}

int open_sync (const char *path)
{
    int rc = 0;
    const char *dyad_path = NULL;
    char upath [PATH_MAX] = {'\0'}; // path with the dyad_path prefix removed

    if (!(dyad_path = getenv (DYAD_PATH_CONS_ENV))) {
        IPRINTF ("DYAD_SYNC OPEN not enabled. Opening \"%s\".\n", path);
        goto done;
    }

    if (cmp_canonical_path_prefix (dyad_path, path, upath, PATH_MAX)) {
        rc = dyad_open_sync (path, dyad_path, upath); // annotate
    } else {
        IPRINTF ("DYAD_SYNC OPEN: %s is not a prefix of %s.\n", \
                 dyad_path, path);
    }

done:
    if (rc == 0 && (ctx && ctx->check))
        setenv (DYAD_CHECK_ENV, "ok", 1);
    return rc;
}

int close_sync (const char *path)
{
    int rc = 0;
    const char *dyad_path = NULL;
    char upath [PATH_MAX] = {'\0'};

    if (!(dyad_path = getenv (DYAD_PATH_PROD_ENV))) {
        IPRINTF ("DYAD_SYNC CLOSE not enabled. Closing \"%s\".\n", path);
        goto done;
    }

    if (cmp_canonical_path_prefix (dyad_path, path, upath, PATH_MAX)) {
        rc = dyad_close_sync (path, dyad_path, upath);
    } else {
        IPRINTF ("DYAD_SYNC CLOSE: %s is not a prefix of %s.\n", \
                 dyad_path, path);
    }

done:
    if (rc == 0 && (ctx && ctx->check))
        setenv (DYAD_CHECK_ENV, "ok", 1);

    return rc;
}


/*****************************************************************************
 *                                                                           *
 *         DYAD Sync Constructor, Destructor and Wrapper API                 *
 *                                                                           *
 *****************************************************************************/

void dyad_sync_init (void)
{
    char *e = NULL;

    if (ctx != NULL) {
        if (ctx->initialized) {
            IPRINTF ("DYAD_WRAPPER: Already initialized.\n");
        } else {
            *ctx = dyad_sync_ctx_t_default;
        }
        return;
    }

    DPRINTF ("DYAD_WRAPPER: Initializeing DYAD wrapper\n");

    if (!(ctx = (dyad_sync_ctx_t *) malloc (sizeof (dyad_sync_ctx_t))))
        exit (1);

    *ctx = dyad_sync_ctx_t_default;

    if ((e = getenv ("DYAD_SYNC_DEBUG"))) {
        ctx->debug = true;
        enable_debug_dyad_utils ();
    } else {
        ctx->debug = false;
        disable_debug_dyad_utils ();
    }

    if ((e = getenv ("DYAD_SYNC_CHECK")))
        ctx->check = true;
    else
        ctx->check = false;

    if ((e = getenv ("DYAD_SHARED_STORAGE")))
        ctx->shared_storage = true;
    else
        ctx->shared_storage = false;

    if ((e = getenv ("DYAD_KEY_DEPTH")))
        ctx->key_depth = atoi (e);
    else
        ctx->key_depth = 3;

    if ((e = getenv ("DYAD_KEY_BINS")))
        ctx->key_bins = atoi (e);
    else
        ctx->key_bins = 1024;

    ctx->reenter = true;

    if ((e = getenv ("DYAD_KVS_NAMESPACE")))
        ctx->kvs_namespace = e;
    else
        ctx->kvs_namespace = NULL;

    if (!(ctx->h = flux_open (NULL, 0))) {
        DPRINTF ("DYAD_SYNC: can't open flux/\n");
    }

    if (flux_get_rank (ctx->h, &ctx->rank) < 0) {
        FLUX_LOG_ERR ("flux_get_rank() failed.\n");
    }

    ctx->initialized = true;

    FLUX_LOG_INFO ("DYAD Initialized\n");
    FLUX_LOG_INFO ("DYAD_SYNC_DEBUG=%s\n", (ctx->debug)? "true": "false");
    FLUX_LOG_INFO ("DYAD_SYNC_CHECK=%s\n", (ctx->check)? "true": "false");
    FLUX_LOG_INFO ("DYAD_KEY_DEPTH=%u\n", ctx->key_depth);
    FLUX_LOG_INFO ("DYAD_KEY_BINS=%u\n", ctx->key_bins);

  #if DYAD_SYNC_START
    ctx->sync_started = false;
    if ((e = getenv ("DYAD_SYNC_START")) && (atoi (e) > 0)) {
        FLUX_LOG_INFO ("Before barrier %u\n", ctx->rank);
        flux_future_t *fb;
        if (!(fb = flux_barrier (ctx->h, "sync_start", atoi (e))))
            FLUX_LOG_ERR ("flux_barrier failed for %d ranks\n", atoi (e));
        if (flux_future_get (fb, NULL) < 0)
            FLUX_LOG_ERR ("flux_future_get for barrir failed\n");
        FLUX_LOG_INFO ("After barrier %u\n", ctx->rank);
        flux_future_destroy (fb);

        ctx->sync_started = true;
        struct timespec t_now;
        clock_gettime (CLOCK_REALTIME, &t_now);
        char tbuf[100];
        strftime (tbuf, sizeof (tbuf), "%D %T", gmtime (&(t_now.tv_sec)));
        printf ("DYAD synchronized start at %s.%09ld\n",
                 tbuf, t_now.tv_nsec);
    }
  #endif // DYAD_SYNC_START
}

void dyad_sync_fini ()
{
  #if DYAD_SYNC_START
    if (ctx->sync_started) {
        struct timespec t_now;
        clock_gettime (CLOCK_REALTIME, &t_now);
        char tbuf[100];
        strftime (tbuf, sizeof (tbuf), "%D %T", gmtime (&(t_now.tv_sec)));
        printf ("DYAD stops at %s.%09ld\n",
                 tbuf, t_now.tv_nsec);
    }
  #endif // DYAD_SYNC_START
    free (ctx);
}

int open (const char *path, int oflag, ...)
{
    char *error = NULL;
    typedef int (*open_ptr_t) (const char *, int, mode_t, ...);
    open_ptr_t func_ptr = NULL;
    int mode = 0;

    if (oflag & O_CREAT)
    {
        va_list arg;
        va_start (arg, oflag);
        mode = va_arg (arg, int);
        va_end (arg);
    }

    func_ptr = (open_ptr_t) dlsym (RTLD_NEXT, "open");
    if ((error = dlerror ())) {
        DPRINTF ("DYAD_SYNC: error in dlsym: %s\n", error);
        return -1;
    }

    if ((mode != O_RDONLY) || is_path_dir (path)) {
        // TODO: make sure if the directory mode is consistent
        goto real_call;
    }

    if (!(ctx && ctx->h) || (ctx && !ctx->reenter)) {
        IPRINTF ("DYAD_SYNC: open sync not applicable for \"%s\".\n", path);
        goto real_call;
    }

    IPRINTF ("DYAD_SYNC: enters open sync (\"%s\").\n", path);
    if (open_sync (path) < 0) {
        DPRINTF ("DYAD_SYNC: failed open sync (\"%s\").\n", path);
        goto real_call;
    }
    IPRINTF ("DYAD_SYNC: exists open sync (\"%s\").\n", path);

real_call:;

    return (func_ptr (path, oflag, mode));
}

FILE *fopen (const char *path, const char *mode)
{
    char *error = NULL;
    typedef FILE *(*fopen_ptr_t) (const char *, const char *);
    fopen_ptr_t func_ptr = NULL;

    func_ptr = (fopen_ptr_t) dlsym (RTLD_NEXT, "fopen");
    if ((error = dlerror ())) {
        DPRINTF ("DYAD_SYNC: error in dlsym: %s\n", error);
        return NULL;
    }

    if ((strcmp (mode, "r") != 0) || is_path_dir (path)) {
        // TODO: make sure if the directory mode is consistent
        goto real_call;
    }

    if (!(ctx && ctx->h) || (ctx && !ctx->reenter) || !path) {
        IPRINTF ("DYAD_SYNC: fopen sync not applicable for \"%s\".\n",
                 ((path)? path : ""));
        goto real_call;
    }

    IPRINTF ("DYAD_SYNC: enters fopen sync (\"%s\").\n", path);
    if (open_sync (path) < 0) {
        DPRINTF ("DYAD_SYNC: failed fopen sync (\"%s\").\n", path);
        goto real_call;
    }
    IPRINTF ("DYAD_SYNC: exits fopen sync (\"%s\").\n", path);

real_call:
    return (func_ptr (path, mode));
}

int close (int fd)
{
    bool to_sync = false;
    char *error = NULL;
    typedef int (*close_ptr_t) (int);
    close_ptr_t func_ptr = NULL;
    char path [PATH_MAX+1] = {'\0'};
    int rc = 0;

    func_ptr = (close_ptr_t) dlsym (RTLD_NEXT, "close");
    if ((error = dlerror ())) {
        DPRINTF ("DYAD_SYNC: error in dlsym: %s\n", error);
        return -1; // return the failure code
    }

    if ((fd < 0) || (ctx == NULL) || (ctx->h == NULL) || !ctx->reenter) {
      #if defined(IPRINTF_DEFINED)
        if (ctx == NULL) {
            IPRINTF ("DYAD_SYNC: close sync not applicable. (no context)\n");
        } else if (ctx->h == NULL) {
            IPRINTF ("DYAD_SYNC: close sync not applicable. (no flux)\n");
        } else if (!ctx->reenter) {
            IPRINTF ("DYAD_SYNC: close sync not applicable. (no reenter)\n");
        } else if (fd >= 0) {
            IPRINTF ("DYAD_SYNC: close sync not applicable. (invalid file descriptor)\n");
        }
      #endif // defined(IPRINTF_DEFINED)
        goto real_call;
    }

    if (is_fd_dir (fd)) {
        // TODO: make sure if the directory mode is consistent
        goto real_call;
    }

    if (get_path (fd, PATH_MAX-1, path) < 0) {
        IPRINTF ("DYAD_SYNC: unable to obtain file path from a descriptor.\n");
        goto real_call;
    }

    to_sync = true;

real_call:; // semicolon here to avoid the error
    // "a label can only be part of a statement and a declaration is not a statement"
    fsync (fd);

  #if DYAD_SYNC_DIR
    if (to_sync) {
        sync_directory (path);
    }
  #endif // DYAD_SYNC_DIR

    int wronly = is_wronly (fd);

    if (wronly == -1)
        DPRINTF ("Failed to check the mode of the file with fcntl: %s\n", strerror (errno));

    if (to_sync && wronly == 1) {
        rc = func_ptr (fd);
        if (rc != 0) {
            DPRINTF ("Failed close (\"%s\").: %s\n", path, strerror (errno));
        }
        IPRINTF ("DYAD_SYNC: enters close sync (\"%s\").\n", path);
        if (close_sync (path) < 0) {
            DPRINTF ("DYAD_SYNC: failed close sync (\"%s\").\n", path);
        }
        IPRINTF ("DYAD_SYNC: exits close sync (\"%s\").\n", path);
    } else {
        rc = func_ptr (fd);
    }

    return rc;
}

int fclose (FILE *fp)
{
    bool to_sync = false;
    char *error = NULL;
    typedef int (*fclose_ptr_t) (FILE *);
    fclose_ptr_t func_ptr = NULL;
    char path [PATH_MAX+1] = {'\0'};
    int rc = 0;
    int fd = 0;

    func_ptr = (fclose_ptr_t) dlsym (RTLD_NEXT, "fclose");
    if ((error = dlerror ())) {
        DPRINTF ("DYAD_SYNC: error in dlsym: %s\n", error);
        return EOF; // return the failure code
    }

    if ((fp == NULL) || (ctx == NULL) || (ctx->h == NULL) || !ctx->reenter) {
      #if defined(IPRINTF_DEFINED)
        if (ctx == NULL) {
            IPRINTF ("DYAD_SYNC: fclose sync not applicable. (no context)\n");
        } else if (ctx->h == NULL) {
            IPRINTF ("DYAD_SYNC: fclose sync not applicable. (no flux)\n");
        } else if (!ctx->reenter) {
            IPRINTF ("DYAD_SYNC: fclose sync not applicable. (no reenter)\n");
        } else if (fp == NULL) {
            IPRINTF ("DYAD_SYNC: fclose sync not applicable. (invalid file pointer)\n");
        }
      #endif // defined(IPRINTF_DEFINED)
        goto real_call;
    }

    if (is_fd_dir (fileno (fp))) {
        // TODO: make sure if the directory mode is consistent
        goto real_call;
    }

    if (get_path (fileno (fp), PATH_MAX-1, path) < 0) {
        IPRINTF ("DYAD_SYNC: unable to obtain file path from a descriptor.\n");
        goto real_call;
    }

    to_sync = true;

real_call:;
    fflush (fp);
    fd = fileno (fp);
    fsync (fd);

  #if DYAD_SYNC_DIR
    if (to_sync) {
        sync_directory (path);
    }
  #endif // DYAD_SYNC_DIR

    int wronly = is_wronly (fd);

    if (wronly == -1)
        DPRINTF ("Failed to check the mode of the file with fcntl: %s\n", strerror (errno));


    if (to_sync && wronly == 1) {
        rc = func_ptr (fp);
        if (rc != 0) {
            DPRINTF ("Failed fclose (\"%s\").\n", path);
        }
        IPRINTF ("DYAD_SYNC: enters fclose sync (\"%s\").\n", path);
        if (close_sync (path) < 0) {
            DPRINTF ("DYAD_SYNC: failed fclose sync (\"%s\").\n", path);
        }
        IPRINTF ("DYAD_SYNC: exits fclose sync (\"%s\").\n", path);
    } else {
        rc = func_ptr (fp);
    }

    return rc;
}

int open_real (const char *path, int oflag, ...)
{
    typedef int (*open_real_ptr_t) (const char *, int, mode_t, ...);
    open_real_ptr_t func_ptr = (open_real_ptr_t) dlsym (RTLD_NEXT, "open");
    char *error = NULL;

    if ((error = dlerror ())) {
        DPRINTF ("DYAD: open() error in dlsym: %s\n", error);
        return -1;
    }

    int mode = 0;

    if (oflag & O_CREAT)
    {
        va_list arg;
        va_start (arg, oflag);
        mode = va_arg (arg, int);
        va_end (arg);
    }

    return (func_ptr (path, oflag, mode));
}

FILE *fopen_real (const char *path, const char *mode)
{
    typedef FILE *(*fopen_real_ptr_t) (const char *, const char *);
    fopen_real_ptr_t func_ptr = (fopen_real_ptr_t) dlsym (RTLD_NEXT, "fopen");
    char *error = NULL;

    if ((error = dlerror ())) {
        DPRINTF ("DYAD: fopen() error in dlsym: %s\n", error);
        return NULL;
    }

    return (func_ptr (path, mode));
}

int close_real (int fd)
{
    typedef int (*close_real_ptr_t) (int);
    close_real_ptr_t func_ptr = (close_real_ptr_t) dlsym (RTLD_NEXT, "close");
    char *error = NULL;

    if ((error = dlerror ())) {
        DPRINTF ("DYAD: close() error in dlsym: %s\n", error);
        return -1; // return the failure code
    }

    return func_ptr (fd);
}

int fclose_real (FILE *fp)
{
    typedef int (*fclose_real_ptr_t) (FILE *);
    fclose_real_ptr_t func_ptr = (fclose_real_ptr_t) dlsym (RTLD_NEXT, "fclose");
    char *error = NULL;

    if ((error = dlerror ())) {
        DPRINTF ("DYAD: fclose() error in dlsym: %s\n", error);
        return EOF; // return the failure code
    }

    return func_ptr (fp);
}

#if DYAD_SYNC_DIR
int sync_directory (const char* path)
{ // Flush new directory entry https://lwn.net/Articles/457671/
    char path_copy [PATH_MAX+1] = {'\0'};
    int odir_fd = -1;
    char *odir = NULL;
    bool reenter = false;
    int rc = 0;

    strncpy (path_copy, path, PATH_MAX);
    odir = dirname (path_copy);

    reenter = ctx->reenter; // backup ctx->reenter
    if (ctx != NULL) ctx->reenter = false;

    if ((odir_fd = open_real (odir, O_RDONLY)) < 0) {
        IPRINTF ("Cannot open the directory \"%s\"\n", odir);
        rc = -1;
    } else {
        if (fsync (odir_fd) < 0) {
            IPRINTF ("Cannot flush the directory \"%s\"\n", odir);
            rc = -1;
        }
        if (close_real (odir_fd) < 0) {
            IPRINTF ("Cannot close the directory \"%s\"\n", odir);
            rc = -1;
        }
    }
    if (ctx != NULL) ctx->reenter = reenter;
    return rc;
}
#endif // DYAD_SYNC_DIR

/*
 * vi: ts=4 sw=4 expandtab
 */
