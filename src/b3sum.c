/* A small, complete b3sum-style example for BLAKE3-Servil's C API.
 * POSIX C11; the library supplies hashing, modes and XOF output.
 */
#define _POSIX_C_SOURCE 200809L
#include <b3.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define PROGRAM "b3sum-c"
#define VERSION "0.1.0"
#define IO_BYTES (64 * 1024)

typedef struct {
    b3_mode mode;
    uint64_t seek;
    size_t length;
    size_t threads;
    bool no_mmap;
    bool no_names;
    bool raw;
    bool tag;
    bool check;
    bool quiet;
    bool library_mt;
} Options;

typedef struct {
    char *path;
    unsigned char *digest;
    char *error;
} Result;

typedef struct {
    char **v;
    size_t n;
    size_t cap;
} Paths;

typedef struct {
    const Options *options;
    Result *results;
    size_t count;
    size_t next;
    pthread_mutex_t lock;
} Workers;

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, PROGRAM ": out of memory\n");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s) {
    char *copy = strdup(s);
    if (!copy) {
        fprintf(stderr, PROGRAM ": out of memory\n");
        exit(2);
    }
    return copy;
}

static char *error_string(const char *fmt, ...) {
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0) {
        va_end(ap);
        return xstrdup("unknown error");
    }
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static void paths_push(Paths *paths, const char *path) {
    if (paths->n == paths->cap) {
        size_t cap = paths->cap ? paths->cap * 2 : 32;
        paths->v = realloc(paths->v, cap * sizeof(*paths->v));
        if (!paths->v) {
            fprintf(stderr, PROGRAM ": out of memory\n");
            exit(2);
        }
        paths->cap = cap;
    }
    paths->v[paths->n++] = xstrdup(path);
}

static int compare_strings(const void *a, const void *b) {
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

static char *path_join(const char *parent, const char *child) {
    size_t a = strlen(parent), b = strlen(child);
    bool slash = a && parent[a - 1] != '/';
    char *result = xmalloc(a + b + (slash ? 2 : 1));
    snprintf(result, a + b + (slash ? 2 : 1), "%s%s%s", parent, slash ? "/" : "", child);
    return result;
}

/* Preserve argument order, but make directory traversal deterministic. */
static int collect_path(Paths *paths, const char *path) {
    struct stat st;
    if (strcmp(path, "-") == 0) {
        paths_push(paths, path);
        return 0;
    }
    if (lstat(path, &st) != 0) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISREG(st.st_mode)) {
        paths_push(paths, path);
        return 0;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, PROGRAM ": %s: not a regular file or directory\n", path);
        return 1;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(errno));
        return 1;
    }
    Paths children = {0};
    struct dirent *entry;
    int failed = 0;
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            paths_push(&children, entry->d_name);
        }
    }
    int saved = errno;
    closedir(dir);
    if (saved) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(saved));
        failed = 1;
    }
    qsort(children.v, children.n, sizeof(*children.v), compare_strings);
    for (size_t i = 0; i < children.n; ++i) {
        char *child = path_join(path, children.v[i]);
        failed |= collect_path(paths, child);
        free(child);
        free(children.v[i]);
    }
    free(children.v);
    return failed;
}

static size_t default_threads(void) {
    const char *env = getenv("RAYON_NUM_THREADS");
    if (env && *env) {
        char *end = NULL;
        unsigned long n = strtoul(env, &end, 10);
        if (end != env && *end == '\0' && n > 0) {
            return (size_t)n;
        }
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (size_t)n : 1;
}

static int read_key(b3_mode *mode) {
    unsigned char key[32];
    size_t used = fread(key, 1, sizeof(key), stdin);
    if (used != sizeof(key)) {
        fprintf(stderr, PROGRAM ": expected 32 key bytes from stdin, found %zu\n", used);
        return 1;
    }
    if (fgetc(stdin) != EOF) {
        fprintf(stderr, PROGRAM ": read more than 32 key bytes from stdin\n");
        return 1;
    }
    *mode = b3_keyed(key);
    return 0;
}

static int hash_stream(FILE *file, const Options *options, b3_hasher *hasher) {
    unsigned char buffer[IO_BYTES];
    bool mt = options->library_mt;
    while (!feof(file)) {
        size_t n = fread(buffer, 1, sizeof(buffer), file);
        if (n) {
            if (mt) {
                b3_hasher_update_multithreaded(hasher, buffer, n);
            } else {
                b3_hasher_update(hasher, buffer, n);
            }
        }
        if (ferror(file)) {
            fprintf(stderr, PROGRAM ": read: %s\n", strerror(errno));
            return 1;
        }
    }
    return 0;
}

static int hash_fd(int fd, const Options *options, b3_hasher *hasher) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, PROGRAM ": fstat: %s\n", strerror(errno));
        return 1;
    }
    if (!options->no_mmap && S_ISREG(st.st_mode) && st.st_size > 0) {
        if ((uintmax_t)st.st_size > (uintmax_t)SIZE_MAX) {
            fprintf(stderr, PROGRAM ": input is too large to map\n");
            return 1;
        }
        void *mapping = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping != MAP_FAILED) {
            (void)posix_madvise(mapping, (size_t)st.st_size, POSIX_MADV_SEQUENTIAL);
            if (options->library_mt) {
                b3_hasher_update_multithreaded(hasher, mapping, (size_t)st.st_size);
            } else {
                b3_hasher_update(hasher, mapping, (size_t)st.st_size);
            }
            munmap(mapping, (size_t)st.st_size);
            return 0;
        }
        if (lseek(fd, 0, SEEK_SET) < 0) {
            fprintf(stderr, PROGRAM ": lseek: %s\n", strerror(errno));
            return 1;
        }
    }
    int duplicate = dup(fd);
    if (duplicate < 0) {
        fprintf(stderr, PROGRAM ": dup: %s\n", strerror(errno));
        return 1;
    }
    FILE *file = fdopen(duplicate, "rb");
    if (!file) {
        close(duplicate);
        fprintf(stderr, PROGRAM ": fdopen: %s\n", strerror(errno));
        return 1;
    }
    int result = hash_stream(file, options, hasher);
    fclose(file);
    return result;
}

static unsigned char *finish(b3_hasher *hasher, const Options *options) {
    unsigned char *out = xmalloc(options->length);
    if (options->length == 32 && options->seek == 0) {
        b3_digest digest = b3_hasher_finalize(hasher);
        memcpy(out, digest.bytes, 32);
    } else {
        b3_hasher_output(hasher, options->seek, out, options->length);
    }
    return out;
}

static int hash_path(const char *path, const Options *options, Result *result) {
    b3_hasher hasher;
    b3_hasher_init(&hasher, &options->mode);
    if (!result->path) {
        result->path = xstrdup(path);
    }
    result->error = NULL;
    result->digest = NULL;
    if (strcmp(path, "-") == 0) {
        if (hash_stream(stdin, options, &hasher) != 0) {
            result->error = error_string("read standard input failed");
            return 1;
        }
    } else {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            result->error = error_string("%s", strerror(errno));
            return 1;
        }
        if (hash_fd(fd, options, &hasher) != 0) {
            close(fd);
            result->error = error_string("%s", strerror(errno));
            return 1;
        }
        close(fd);
    }
    result->digest = finish(&hasher, options);
    return 0;
}

static void *worker_main(void *argument) {
    Workers *workers = argument;
    for (;;) {
        pthread_mutex_lock(&workers->lock);
        size_t i = workers->next++;
        pthread_mutex_unlock(&workers->lock);
        if (i >= workers->count) {
            break;
        }
        (void)hash_path(workers->results[i].path, workers->options, &workers->results[i]);
    }
    return NULL;
}

static int hash_all(const Options *options, Result *results, size_t count) {
    size_t threads = options->no_mmap ? 1 : options->threads;
    if (threads > count) {
        threads = count;
    }
    if (threads <= 1) {
        for (size_t i = 0; i < count; ++i) {
            (void)hash_path(results[i].path, options, &results[i]);
        }
        return 0;
    }
    Workers workers = {.options = options, .results = results, .count = count, .next = 0};
    if (pthread_mutex_init(&workers.lock, NULL) != 0) {
        fprintf(stderr, PROGRAM ": cannot initialize worker lock\n");
        return 1;
    }
    pthread_t *ids = xmalloc((threads - 1) * sizeof(*ids));
    size_t made = 0;
    for (; made < threads - 1; ++made) {
        int e = pthread_create(&ids[made], NULL, worker_main, &workers);
        if (e != 0) {
            fprintf(stderr, PROGRAM ": pthread_create: %s\n", strerror(e));
            break;
        }
    }
    if (made != threads - 1) {
        for (size_t i = 0; i < made; ++i) {
            pthread_join(ids[i], NULL);
        }
        free(ids);
        pthread_mutex_destroy(&workers.lock);
        return 1;
    }
    worker_main(&workers);
    for (size_t i = 0; i < made; ++i) {
        pthread_join(ids[i], NULL);
    }
    free(ids);
    pthread_mutex_destroy(&workers.lock);
    return 0;
}

static char hex_digit(unsigned value) {
    return "0123456789abcdef"[value & 15];
}

static char *hex(const unsigned char *bytes, size_t length) {
    char *out = xmalloc(length * 2 + 1);
    for (size_t i = 0; i < length; ++i) {
        out[2 * i] = hex_digit(bytes[i] >> 4);
        out[2 * i + 1] = hex_digit(bytes[i]);
    }
    out[length * 2] = '\0';
    return out;
}

/* b3sum's portable check-file escaping: prefix a backslash when escaped. */
static char *display_path(const char *path) {
    size_t n = strlen(path), extra = 0;
    for (size_t i = 0; i < n; ++i) {
        if (path[i] == '\\' || path[i] == '\n' || path[i] == '\r') {
            ++extra;
        }
    }
    char *out = xmalloc(n + extra + 1);
    char *at = out;
    for (size_t i = 0; i < n; ++i) {
        if (path[i] == '\\') {
            *at++ = '\\';
            *at++ = '\\';
        } else if (path[i] == '\n') {
            *at++ = '\\';
            *at++ = 'n';
        } else if (path[i] == '\r') {
            *at++ = '\\';
            *at++ = 'r';
        } else {
            *at++ = path[i];
        }
    }
    *at = '\0';
    return out;
}

static int print_result(const Result *result, const Options *options) {
    char *path = display_path(result->path);
    if (result->error) {
        fprintf(stderr, PROGRAM ": %s: %s\n", result->path, result->error);
        free(path);
        return 1;
    }
    if (options->raw) {
        if (options->length && fwrite(result->digest, 1, options->length, stdout) != options->length) {
            free(path);
            return 1;
        }
    } else {
        bool escaped = strchr(result->path, '\\') || strchr(result->path, '\n') || strchr(result->path, '\r');
        if (escaped) {
            fputc('\\', stdout);
        }
        char *digest = hex(result->digest, options->length);
        if (options->tag) {
            printf("BLAKE3 (%s) = %s\n", path, digest);
        } else if (options->no_names) {
            printf("%s\n", digest);
        } else {
            printf("%s  %s\n", digest, path);
        }
        free(digest);
    }
    free(path);
    return ferror(stdout) ? 1 : 0;
}

static int parse_u64(const char *text, uint64_t *value) {
    if (!text || !*text || *text == '-') {
        return 1;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || end == text || *end) {
        return 1;
    }
    *value = (uint64_t)n;
    return 0;
}

static int usage(FILE *out) {
    fprintf(out,
            "Usage: " PROGRAM " [OPTIONS] [FILE]...\n"
            "Hash files, directories, or standard input with BLAKE3-Servil.\n\n"
            "  --keyed                 read a 32-byte key from stdin\n"
            "  --derive-key CONTEXT   use key derivation with CONTEXT\n"
            "  -l, --length N         output N bytes (default 32)\n"
            "  --seek N              start output at byte N (default 0)\n"
            "  --num-threads N       maximum threads; 0 uses the default\n"
            "  --no-mmap             read instead of mapping; single-threaded\n"
            "  --no-names            omit filenames\n"
            "  --raw                 raw output bytes; one input only\n"
            "  --tag                 BSD format: BLAKE3 (FILE) = HASH\n"
            "  -c, --check           read checksum lines and verify them\n"
            "  --quiet               suppress OK lines with --check\n"
            "  -h, --help            this help\n"
            "  -V, --version         version\n");
    return out == stdout ? 0 : 2;
}

static int parse_options(int argc, char **argv, Options *options, int *first) {
    *first = 1;
    options->length = 32;
    options->threads = 0;
    bool keyed = false;
    const char *derive = NULL;
    bool end = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!end && strcmp(arg, "--") == 0) {
            end = true;
            *first = i + 1;
            break;
        }
        if (end || arg[0] != '-' || !arg[1]) {
            *first = i;
            break;
        }
        const char *value = NULL;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            return usage(stdout);
        }
        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            printf(PROGRAM " %s (BLAKE3-Servil %s)\n", VERSION, b3_version());
            return 0;
        }
        if (strcmp(arg, "--keyed") == 0) {
            keyed = true;
        } else if (strncmp(arg, "--derive-key", 12) == 0 && (arg[12] == '\0' || arg[12] == '=')) {
            derive = arg[12] == '=' ? arg + 13 : (++i < argc ? argv[i] : NULL);
            if (!derive) {
                return usage(stderr);
            }
        } else if (strcmp(arg, "--no-mmap") == 0) {
            options->no_mmap = true;
        } else if (strcmp(arg, "--no-names") == 0) {
            options->no_names = true;
        } else if (strcmp(arg, "--raw") == 0) {
            options->raw = true;
        } else if (strcmp(arg, "--tag") == 0) {
            options->tag = true;
        } else if (strcmp(arg, "--check") == 0 || strcmp(arg, "-c") == 0) {
            options->check = true;
        } else if (strcmp(arg, "--quiet") == 0) {
            options->quiet = true;
        } else if (strcmp(arg, "-l") == 0 || strncmp(arg, "--length", 8) == 0 || strncmp(arg, "--seek", 6) == 0 || strncmp(arg, "--num-threads", 13) == 0) {
            bool length = strcmp(arg, "-l") == 0 || strncmp(arg, "--length", 8) == 0;
            bool seek = !length && strncmp(arg, "--seek", 6) == 0;
            size_t prefix = length ? (arg[1] == '-' ? 8 : 2) : (seek ? 6 : 13);
            if (arg[prefix] == '=') {
                value = arg + prefix + 1;
            } else if (arg[prefix] == '\0') {
                value = ++i < argc ? argv[i] : NULL;
            } else {
                return usage(stderr);
            }
            uint64_t number;
            if (!value || parse_u64(value, &number)) {
                return usage(stderr);
            }
            if (length) {
                if (number > SIZE_MAX) {
                    fprintf(stderr, PROGRAM ": output length does not fit this platform\n");
                    return 2;
                }
                options->length = (size_t)number;
            } else if (seek) {
                options->seek = number;
            } else {
                if (number > SIZE_MAX) {
                    fprintf(stderr, PROGRAM ": thread count does not fit this platform\n");
                    return 2;
                }
                options->threads = (size_t)number;
            }
        } else {
            fprintf(stderr, PROGRAM ": unknown option: %s\n", arg);
            return 2;
        }
    }
    if (keyed && derive) {
        fprintf(stderr, PROGRAM ": --keyed and --derive-key cannot be used together\n");
        return 2;
    }
    if (options->quiet && !options->check) {
        fprintf(stderr, PROGRAM ": --quiet requires --check\n");
        return 2;
    }
    if (options->raw && (options->check || options->tag)) {
        fprintf(stderr, PROGRAM ": --raw cannot be used with --check or --tag\n");
        return 2;
    }
    if (options->no_names && options->check) {
        fprintf(stderr, PROGRAM ": --no-names cannot be used with --check\n");
        return 2;
    }
    if (options->check && (options->length != 32 || options->seek != 0)) {
        fprintf(stderr, PROGRAM ": --check requires the default 32-byte output at offset zero\n");
        return 2;
    }
    if (keyed) {
        options->mode = b3_plain();
        /* Read after all arguments are known: '-' is not a keyed input. */
        for (int i = *first; i < argc; ++i) {
            if (strcmp(argv[i], "-") == 0) {
                fprintf(stderr, PROGRAM ": cannot use '-' as an input in keyed mode\n");
                return 2;
            }
        }
        if (read_key(&options->mode) != 0) {
            return 2;
        }
    } else if (derive) {
        options->mode = b3_derive_key(derive);
    } else {
        options->mode = b3_plain();
    }
    if (options->threads == 0) {
        options->threads = default_threads();
    }
    /* The C API has no thread-count setter. Default calls may use its pool;
     * non-default values are enforced by this program's worker count. */
    options->library_mt = !options->no_mmap && options->threads != 1 && !getenv("RAYON_NUM_THREADS") && options->threads == default_threads();
    if (*first >= argc && options->check && keyed) {
        fprintf(stderr, PROGRAM ": a checkfile on stdin is not available in keyed mode\n");
        return 2;
    }
    return 1;
}

static char *unescape_path(const char *text) {
    size_t n = strlen(text);
    char *out = xmalloc(n + 1), *at = out;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] != '\\') {
            *at++ = text[i];
            continue;
        }
        if (++i >= n) {
            free(out);
            return NULL;
        }
        if (text[i] == 'n') {
            *at++ = '\n';
        } else if (text[i] == 'r') {
            *at++ = '\r';
        } else if (text[i] == '\\') {
            *at++ = '\\';
        } else {
            free(out);
            return NULL;
        }
    }
    *at = '\0';
    return out;
}

static int parse_check_line(const char *line, char **path, char **display, unsigned char expected[32]) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
        --n;
    }
    if (!n) {
        return 1;
    }
    bool escaped = line[0] == '\\';
    const char *start = line + (escaped ? 1 : 0);
    const char *split = strstr(start, "  ");
    const char *hash_start = NULL, *path_start = NULL;
    size_t hash_len = 0, path_len = 0;
    if (split) {
        hash_start = start;
        hash_len = (size_t)(split - start);
        path_start = split + 2;
        path_len = n - (size_t)(path_start - line);
    } else if (strncmp(start, "BLAKE3 (", 8) == 0) {
        const char *end = NULL;
        for (const char *p = start + 8; (p = strstr(p, ") = ")) != NULL; p += 4) {
            end = p;
        }
        if (end) {
            path_start = start + 8;
            path_len = (size_t)(end - path_start);
            hash_start = end + 4;
            hash_len = n - (size_t)(hash_start - line);
        }
    }
    if (!hash_start || hash_len != 64 || path_len == 0) {
        return 1;
    }
    for (size_t i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (int j = 0; j < 2; ++j) {
            char c = hash_start[2 * i + (size_t)j];
            if (c >= '0' && c <= '9') {
                value = value * 16 + (unsigned)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value = value * 16 + (unsigned)(c - 'a' + 10);
            } else {
                return 1;
            }
        }
        expected[i] = (unsigned char)value;
    }
    char *copy = xmalloc(path_len + 1);
    memcpy(copy, path_start, path_len);
    copy[path_len] = '\0';
    *path = escaped ? unescape_path(copy) : xstrdup(copy);
    *display = xmalloc(strlen(copy) + 2);
    sprintf(*display, "%s%s", escaped ? "\\" : "", copy);
    free(copy);
    if (!*path) {
        free(*display);
        *display = NULL;
        return 1;
    }
    return 0;
}

static int check_stream(FILE *file, const Options *options) {
    char *line = NULL;
    size_t capacity = 0;
    ssize_t n;
    int failures = 0;
    while ((n = getline(&line, &capacity, file)) >= 0) {
        if (memchr(line, '\0', (size_t)n)) {
            fprintf(stderr, PROGRAM ": NUL in check line\n");
            ++failures;
            continue;
        }
        char *path = NULL;
        char *display = NULL;
        unsigned char expected[32];
        if (parse_check_line(line, &path, &display, expected) != 0) {
            fprintf(stderr, PROGRAM ": invalid check line\n");
            ++failures;
            continue;
        }
        Result result = {0};
        int failed = hash_path(path, options, &result);
        if (failed || memcmp(expected, result.digest, 32) != 0) {
            printf("%s: FAILED\n", display);
            ++failures;
            if (result.error) {
                fprintf(stderr, PROGRAM ": %s: %s\n", path, result.error);
            }
        } else if (!options->quiet) {
            printf("%s: OK\n", display);
        }
        free(result.path);
        free(result.digest);
        free(result.error);
        free(path);
        free(display);
    }
    free(line);
    if (ferror(file)) {
        fprintf(stderr, PROGRAM ": read checkfile: %s\n", strerror(errno));
        ++failures;
    }
    return failures ? 1 : 0;
}

static int check_path(const char *path, const Options *options) {
    if (strcmp(path, "-") == 0) {
        return check_stream(stdin, options);
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, PROGRAM ": %s: %s\n", path, strerror(errno));
        return 1;
    }
    int result = check_stream(file, options);
    fclose(file);
    return result;
}

int main(int argc, char **argv) {
    Options options = {0};
    int first = 1;
    int parsed = parse_options(argc, argv, &options, &first);
    if (parsed != 1) {
        return parsed;
    }
    Paths inputs = {0};
    int failures = 0;
    if (first == argc) {
        paths_push(&inputs, "-");
    } else {
        for (int i = first; i < argc; ++i) {
            failures |= collect_path(&inputs, argv[i]);
        }
    }
    if (options.raw && inputs.n != 1) {
        fprintf(stderr, PROGRAM ": --raw accepts exactly one input\n");
        failures = 1;
    }
    if (options.check) {
        for (size_t i = 0; i < inputs.n; ++i) {
            failures |= check_path(inputs.v[i], &options);
        }
    } else if (!failures) {
        Result *results = xmalloc(inputs.n * sizeof(*results));
        for (size_t i = 0; i < inputs.n; ++i) {
            results[i].path = inputs.v[i];
        }
        failures |= hash_all(&options, results, inputs.n);
        for (size_t i = 0; i < inputs.n; ++i) {
            failures |= print_result(&results[i], &options);
            free(results[i].digest);
            free(results[i].error);
        }
        free(results);
    }
    for (size_t i = 0; i < inputs.n; ++i) {
        free(inputs.v[i]);
    }
    free(inputs.v);
    return failures ? 1 : 0;
}
