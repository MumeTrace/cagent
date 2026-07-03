#include "ca_platform.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>

static void ca_platform_free_partial(int argc, char **argv)
{
    int i;

    if (argv == NULL) {
        return;
    }

    for (i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}
#endif

ca_status_t ca_platform_argv_to_utf8(int argc, char **argv, int *out_argc, char ***out_argv)
{
    if (out_argc == NULL || out_argv == NULL) {
        return CA_ERR_INVALID_ARG;
    }

#ifdef _WIN32
    {
        wchar_t **wide_argv;
        int wide_argc = 0;
        char **utf8_argv;
        int i;

        /*
         * Windows process argv often follows the active code page. Reading the
         * wide command line and converting once to UTF-8 keeps all later C code
         * model/provider-facing instead of console-codepage-facing.
         */
        wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
        if (wide_argv == NULL || wide_argc <= 0) {
            *out_argc = argc;
            *out_argv = argv;
            return CA_OK;
        }

        utf8_argv = (char **)calloc((size_t)wide_argc + 1u, sizeof(*utf8_argv));
        if (utf8_argv == NULL) {
            LocalFree(wide_argv);
            return CA_ERR_NO_MEMORY;
        }

        for (i = 0; i < wide_argc; i++) {
            int needed = WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, NULL, 0, NULL, NULL);
            if (needed <= 0) {
                ca_platform_free_partial(i, utf8_argv);
                LocalFree(wide_argv);
                return CA_ERR_INVALID_ARG;
            }

            utf8_argv[i] = (char *)malloc((size_t)needed);
            if (utf8_argv[i] == NULL) {
                ca_platform_free_partial(i, utf8_argv);
                LocalFree(wide_argv);
                return CA_ERR_NO_MEMORY;
            }

            if (WideCharToMultiByte(CP_UTF8, 0, wide_argv[i], -1, utf8_argv[i], needed, NULL, NULL) <= 0) {
                ca_platform_free_partial(i + 1, utf8_argv);
                LocalFree(wide_argv);
                return CA_ERR_INVALID_ARG;
            }
        }

        LocalFree(wide_argv);
        *out_argc = wide_argc;
        *out_argv = utf8_argv;
        return CA_OK;
    }
#else
    *out_argc = argc;
    *out_argv = argv;
    return CA_OK;
#endif
}

void ca_platform_argv_free(int argc, char **argv)
{
#ifdef _WIN32
    ca_platform_free_partial(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
}
