#ifndef CPPCC_H
#define CPPCC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CppccDiagnostic {
    char *file;
    int line;
    int column;
    char *message;
} CppccDiagnostic;

enum {
    CPPCC_OK = 0,
    CPPCC_DIAGNOSTICS = 1,
    CPPCC_ERROR = 2
};

/* Compile `path` for `target` ("nuc", "win64" or "linux"). When
 * `output_path` is non-null the image is written there. Diagnostics, when
 * requested, must be released with cppcc_free_diagnostics(). */
int cppcc_compile_file(const char *path,
                       const char *target,
                       const char *output_path,
                       CppccDiagnostic **out_diagnostics,
                       int *out_count);

/* Compile in-memory source and return the image bytes. Both out_bytes and
 * out_len must be non-null. Release with cppcc_free_bytes(). */
int cppcc_compile_source(const char *source,
                         const char *filename,
                         const char *target,
                         unsigned char **out_bytes,
                         size_t *out_len,
                         CppccDiagnostic **out_diagnostics,
                         int *out_count);

void cppcc_free_bytes(unsigned char *bytes, size_t len);
void cppcc_free_diagnostics(CppccDiagnostic *diagnostics, int count);
const char *cppcc_last_error(void);
const char *cppcc_version(void);

#ifdef __cplusplus
}
#endif

#endif /* CPPCC_H */
