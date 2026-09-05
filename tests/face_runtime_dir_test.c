#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int zh_face_test_mkdir_recursive(const char *dir_path);

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int main(void) {
    char root[] = "/tmp/zh_face_dir_test.XXXXXX";
    char nested[256];
    char conflict[256];
    char child[256];
    FILE *fp;

    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }

    snprintf(nested, sizeof(nested), "%s/face/save/", root);
    if (zh_face_test_mkdir_recursive(nested) != 0 || !is_dir(nested)) {
        perror("create nested face directory");
        return 1;
    }
    if (zh_face_test_mkdir_recursive(nested) != 0) {
        perror("recreate existing face directory");
        return 1;
    }

    snprintf(conflict, sizeof(conflict), "%s/not_a_dir", root);
    fp = fopen(conflict, "w");
    if (!fp) {
        perror("create conflict file");
        return 1;
    }
    fclose(fp);
    snprintf(child, sizeof(child), "%s/not_a_dir/save", root);
    errno = 0;
    if (zh_face_test_mkdir_recursive(child) == 0 || errno != ENOTDIR) {
        fprintf(stderr, "expected ENOTDIR for path component conflict, errno=%d\n", errno);
        return 1;
    }

    unlink(conflict);
    snprintf(nested, sizeof(nested), "%s/face/save", root);
    rmdir(nested);
    snprintf(nested, sizeof(nested), "%s/face", root);
    rmdir(nested);
    rmdir(root);
    puts("face runtime directory tests passed");
    return 0;
}
