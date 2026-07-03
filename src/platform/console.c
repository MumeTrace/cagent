#include "ca_platform.h"

#include <locale.h>

#ifdef _WIN32
#include <windows.h>
#endif

ca_status_t ca_console_init(void)
{
    /*
     * The runtime keeps using printf/fgets with narrow char buffers. Setting
     * the Windows console code pages to UTF-8 makes those bytes line up with
     * the internal representation expected by the LLM and JSON layers.
     */
    (void)setlocale(LC_ALL, "");

#ifdef _WIN32
    (void)SetConsoleOutputCP(CP_UTF8);
    (void)SetConsoleCP(CP_UTF8);
#endif

    return CA_OK;
}
