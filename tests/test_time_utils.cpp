#include "core/time_utils.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

int main() {
    char root[] = "/tmp/unified_capture_mkdir_p_XXXXXX";
    assert(mkdtemp(root) != nullptr);

    char nested[512];
    snprintf(nested, sizeof(nested), "%s/a/b", root);
    assert(mkdir_p(nested, 0755) == 0);
    assert(mkdir_p(nested, 0755) == 0);

    struct stat st {};
    assert(stat(nested, &st) == 0);
    assert(S_ISDIR(st.st_mode));

    assert(rmdir(nested) == 0);
    snprintf(nested, sizeof(nested), "%s/a", root);
    assert(rmdir(nested) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
