/*
 * explorer_order.h - Read the on-screen file order of an open File Explorer window.
 *
 * Windows XP through Windows 11 (x86, x64, ARM64, ARM).
 * Link with: ole32.lib shell32.lib shlwapi.lib oleaut32.lib uuid.lib user32.lib mpr.lib
 * Callable from C and C++.
 */
#ifndef EXPLORER_ORDER_H
#define EXPLORER_ORDER_H

#include <stddef.h>
#include <wchar.h>

/* Define as __declspec(dllexport) / __declspec(dllimport) when building or using a DLL. */
#ifndef EXPLORER_ORDER_API
#define EXPLORER_ORDER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ExplorerFileList {
    size_t    count;  /* number of entries in paths */
    wchar_t **paths;  /* full paths of all files (no folders) in the order Explorer is displaying them */
    ptrdiff_t index;  /* position of the requested file in paths, or -1 if it isn't listed */
} ExplorerFileList;

/*
 * Finds an open File Explorer window (or tab) showing the folder that contains filePath, and
 * returns the files directly in that folder, as shown in the window, in the order they are
 * displayed, honoring the window's sort and grouping (on Windows XP, grouping is not accounted
 * for). Files in collapsed groups are included. Subfolders are not included (neither the folders
 * themselves nor their contents). .zip files and shortcuts count as files.
 *
 * filePath does not need to exist; only its parent folder is used to pick the window. The result's
 * index field gives filePath's position in paths, or -1 if it isn't there (it doesn't exist, is a
 * folder, or is hidden in that window). Forward slashes, case and 8.3 short names are accepted.
 *
 * If several windows or tabs show that folder, the most recently active one is used (the frontmost
 * window by z-order; within a window, the selected tab). For files on the Desktop, the desktop
 * itself counts as a window behind all others: its icons are listed in on-screen order (down each
 * column, then across).
 *
 * Returns NULL if no matching window is open (or on error). An open window showing a folder with
 * no files returns a list with count == 0.
 * Free the result with ExplorerOrder_Free.
 */
EXPLORER_ORDER_API ExplorerFileList *ExplorerOrder_GetFiles(const wchar_t *filePath);

/* Frees a list returned by ExplorerOrder_GetFiles. Passing NULL is allowed. */
EXPLORER_ORDER_API void ExplorerOrder_Free(ExplorerFileList *list);

#ifdef __cplusplus
}
#endif

#endif /* EXPLORER_ORDER_H */
