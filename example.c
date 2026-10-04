/* example.c - prints the Explorer display order for the folder containing argv[1].
 * Plain C89 so it builds with old compilers. */
#include <stdio.h>
#include <wchar.h>

#include "explorer_order.h"

int wmain(int argc, wchar_t **argv) {
    ExplorerFileList *list;
    size_t i;

    if (argc < 2) {
        fwprintf(stderr, L"usage: %ls <file path>\n", argv[0]);
        return 2;
    }

    list = ExplorerOrder_GetFiles(argv[1]);
    if (!list) {
        wprintf(L"No open Explorer window shows the folder containing that path.\n");
        return 1;
    }

    for (i = 0; i < list->count; ++i)
        wprintf(L"%c %4lu. %ls\n", (ptrdiff_t)i == list->index ? L'>' : L' ', (unsigned long)i, list->paths[i]);
    wprintf(L"index = %ld\n", (long)list->index);

    ExplorerOrder_Free(list);
    return 0;
}
