#include "../include/cppcc.h"
#include <stdio.h>

int main(void) {
    CppccDiagnostic *diagnostics = 0;
    int count = 0;
    int status;

    printf("version=%s\n", cppcc_version());
    status = cppcc_compile_file("../../cppcc/examples/hello.cc", "linux",
                                "/tmp/capi_smoke.elf", &diagnostics, &count);
    if (count > 0) {
        int i;
        for (i = 0; i < count; i++) {
            fprintf(stderr, "%s:%d:%d: %s\n", diagnostics[i].file,
                    diagnostics[i].line, diagnostics[i].column,
                    diagnostics[i].message);
        }
        cppcc_free_diagnostics(diagnostics, count);
    }
    printf("status=%d\n", status);
    return status == CPPCC_OK ? 0 : 1;
}
