#include "../libcommon/common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../libcommon/files.h"
#include "../libcommon/log.h"

#define ARGS_MAX 4096
#define __XSTR(x) #x
#define _XSTR(x) __XSTR(x)
#define LHFUZZ_A_PATH "/tmp/libhfuzz.a"

static bool isCXX = false;
static bool isGCC = false;

__asm__("\n"
        "	.global lhfuzz_start\n"
        "	.global lhfuzz_end\n"
        "lhfuzz_start:\n" "	.incbin \"libhfuzz/libhfuzz.a\"\n" "lhfuzz_end:\n" "\n");

static bool useASAN()
{
    if (getenv("HFUZZ_CC_ASAN") != NULL) {
        return true;
    }
    return false;
}

static bool useMSAN()
{
    if (getenv("HFUZZ_CC_MSAN") != NULL) {
        return true;
    }
    return false;
}

static bool useUBSAN()
{
    if (getenv("HFUZZ_CC_UBSAN") != NULL) {
        return true;
    }
    return false;
}

static bool isLDMode(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            return false;
        }
        if (strcmp(argv[i], "-c") == 0) {
            return false;
        }
        if (strcmp(argv[i], "-E") == 0) {
            return false;
        }
        if (strcmp(argv[i], "-S") == 0) {
            return false;
        }
    }
    return true;
}

static int execCC(int argc, char **argv)
{
    if (useASAN()) {
        argv[argc++] = "-fsanitize=address";
    }
    if (useMSAN()) {
        argv[argc++] = "-fsanitize=memory";
    }
    if (useUBSAN()) {
        argv[argc++] = "-fsanitize=undefined";
    }
    argv[argc] = NULL;

    const char *cc_path = getenv("HFUZZ_CC_PATH");
    if (cc_path != NULL) {
        execvp(cc_path, argv);
        PLOG_E("execvp('%s')", cc_path);
        return EXIT_FAILURE;
    }

    if (isGCC) {
        if (isCXX) {
            execvp("g++-8", argv);
            execvp("g++-7", argv);
            execvp("g++-6", argv);
            execvp("g++-5", argv);
            execvp("g++", argv);
            execvp("gcc", argv);
        } else {
            execvp("gcc-8", argv);
            execvp("gcc-7", argv);
            execvp("gcc-6", argv);
            execvp("gcc-5", argv);
            execvp("gcc", argv);
        }
    } else {
        if (isCXX) {
            execvp("clang++-devel", argv);
            execvp("clang++-6.0", argv);
            execvp("clang++-5.0", argv);
            execvp("clang++-4.0", argv);
            execvp("clang++", argv);
            execvp("clang", argv);
        } else {
            execvp("clang-devel", argv);
            execvp("clang-6.0", argv);
            execvp("clang-5.0", argv);
            execvp("clang-4.0", argv);
            execvp("clang", argv);
        }
    }

    PLOG_E("execvp('%s')", argv[0]);
    return EXIT_FAILURE;
}

static int ccMode(int argc, char **argv)
{
    char *args[4096];

    int j = 0;
    if (isCXX) {
        args[j++] = "c++";
    } else {
        args[j++] = "cc";
    }
    if (isGCC) {
        args[j++] = "-fsanitize-coverage=trace-pc";
    } else {
        args[j++] = "-fsanitize-coverage=trace-pc-guard,trace-cmp,indirect-calls";
        args[j++] = "-mllvm";
        args[j++] = "-sanitizer-coverage-prune-blocks=0";
        args[j++] = "-mllvm";
        args[j++] = "-sanitizer-coverage-block-threshold=10000000";
        args[j++] = "-mllvm";
        args[j++] = "-sanitizer-coverage-level=2";
    }
    args[j++] = "-funroll-loops";
    args[j++] = "-fno-inline";
    args[j++] = "-fno-builtin";

    for (int i = 1; i < argc; i++) {
        args[j++] = argv[i];
    }

    return execCC(j, args);
}

static bool getLibHfuzz(void)
{
    extern uint8_t lhfuzz_start __asm__("lhfuzz_start");
    extern uint8_t lhfuzz_end __asm__("lhfuzz_end");

    ptrdiff_t len = (uintptr_t) & lhfuzz_end - (uintptr_t) & lhfuzz_start;

    /* Does the library exist and is of the expected size */
    struct stat st;
    if (stat(LHFUZZ_A_PATH, &st) != -1) {
        if (st.st_size == len) {
            return true;
        }
    }

    char template[] = "/tmp/libhfuzz.a.XXXXXX";
    int fd = mkostemp(template, O_CLOEXEC);
    if (fd == -1) {
        PLOG_E("mkostemp('%s')", template);
        return false;
    }

    bool ret = files_writeToFd(fd, &lhfuzz_start, len);
    if (!ret) {
        PLOG_E("Couldn't write to '%s'", template);
        close(fd);
        return false;
    }
    close(fd);

    if (rename(template, LHFUZZ_A_PATH) == -1) {
        PLOG_E("Couldn't rename('%s', '%s')", template, LHFUZZ_A_PATH);
        unlink(template);
        return false;
    }

    return true;
}

static int ldMode(int argc, char **argv)
{
    if (!getLibHfuzz()) {
        return EXIT_FAILURE;
    }

    char *args[4096];

    int j = 0;
    if (isCXX) {
        args[j++] = "c++";
    } else {
        args[j++] = "cc";
    }
    args[j++] = "-Wl,-z,muldefs";
    args[j++] = "-Wl,--whole-archive";
    args[j++] = LHFUZZ_A_PATH;
    args[j++] = "-Wl,--no-whole-archive";
    if (isGCC) {
        args[j++] = "-fsanitize-coverage=trace-pc";
    } else {
        args[j++] = "-fsanitize-coverage=trace-pc-guard,trace-cmp,indirect-calls";
        args[j++] = "-mllvm";
        args[j++] = "-sanitizer-coverage-prune-blocks=0";
        args[j++] = "-mllvm";
        args[j++] = "-sanitizer-coverage-block-threshold=10000000";
        args[j++] = "-mllvm";
        args[j++] = "-sanitizer-coverage-level=2";
    }
    args[j++] = "-funroll-loops";
    args[j++] = "-fno-inline";
    args[j++] = "-fno-builtin";

    int i;
    for (i = 1; i < argc; i++) {
        args[j++] = argv[i];
    }
    args[j++] = LHFUZZ_A_PATH;

    return execCC(j, args);
}

int main(int argc, char **argv)
{
    if (strstr(argv[0], "++") != NULL) {
        isCXX = true;
    }
    if (strstr(argv[0], "gcc") != NULL) {
        isGCC = true;
    }
    if (strstr(argv[0], "g++") != NULL) {
        isGCC = true;
    }
    if (argc <= 1) {
        LOG_I("'%s': No arguments provided", argv[0]);
        return execCC(argc, argv);
    }
    if (argc > (ARGS_MAX - 4)) {
        LOG_F("'%s': Too many positional arguments: %d", argv[0], argc);
        return EXIT_FAILURE;
    }

    if (isLDMode(argc, argv)) {
        return ldMode(argc, argv);
    }
    return ccMode(argc, argv);
}
