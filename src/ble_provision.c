#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ble_provision.h"
#include "config.h"
#include "log.h"

static const char *zh_ble_script_path(void) {
    const char *env = getenv("ZH_BLE_PROVISION_SCRIPT");
    if (env && env[0] != '\0') {
        return env;
    }
    if (access("/data/zh_work/start_ble_provision.sh", X_OK) == 0) {
        return "/data/zh_work/start_ble_provision.sh";
    }
    return "scripts/self_start/start_ble_provision.sh";
}

int zh_ble_provision_enter(const char *device_id) {
    const char *script = zh_ble_script_path();
    char name[32] = {0};
    int status = 0;
    pid_t pid;

    if (!device_id || device_id[0] == '\0') {
        snprintf(name, sizeof(name), "zh_unknown");
    } else {
        snprintf(name, sizeof(name), "zh_%s", device_id);
    }

    if (access(script, X_OK) != 0) {
        LOGW(__func__, "ble provision script not executable: %s", script);
        return -1;
    }

    LOGI(__func__, "enter ble provision mode, name=%s", name);
    pid = fork();
    if (pid < 0) {
        LOGE(__func__, "fork failed: errno=%d", errno);
        return -1;
    }
    if (pid == 0) {
        execl("/bin/sh", "sh", script, name, (char *)NULL);
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        LOGE(__func__, "waitpid failed: errno=%d", errno);
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        LOGI(__func__, "ble provision mode exited normally");
        return 0;
    }

    LOGW(__func__, "ble provision mode exited abnormally: status=%d", status);
    return -1;
}
