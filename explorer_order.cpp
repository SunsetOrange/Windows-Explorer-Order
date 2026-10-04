/*
 * explorer_order.cpp - implementation of explorer_order.h for Windows XP through Windows 11.
 *
 * Windows come from ShellWindows; each window's IShellBrowser -> IShellView -> IFolderView gives
 * the folder it shows (as an ID list) and its items. The window whose folder path matches the
 * requested file's folder is chosen (the most recently active one if several match), and its item
 * ID lists are turned into paths by binding to the folder in this process. The requested path is
 * passed through the shell too, so all paths compared are in the shell's form (8.3 names expanded,
 * paths over MAX_PATH shortened).
 *
 * Items are requested in view order (SVGIO_FLAG_VIEWORDER). If the view rejects that request, they
 * are read one by one with IFolderView::Item; XP's folder view keeps its list control sorted, so
 * item index order is the display order there (except when "Show in Groups" is on).
 *
 * Only APIs present on Windows XP are imported. CompareStringOrdinal (Vista+) is looked up at run
 * time and used when available. Written in C++98 without the standard library so it builds with
 * XP-capable toolsets (VS2008, VS2010, v141_xp) and MinGW as well as current compilers.
 */
// Hide declarations newer than Windows XP so anything that wouldn't exist there fails to compile.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif

#include "explorer_order.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shlguid.h>
#include <servprov.h>
#include <winnetwk.h>

#include <limits.h>

namespace {

// Not declared by older SDKs. Views that don't know it reject it, and the code falls back.
const UINT kSvgioFlagViewOrder = 0x80000000;
const int kSwcDesktop = 8;  // SWC_DESKTOP

typedef int (WINAPI *CompareStringOrdinalFn)(const WCHAR *, int, const WCHAR *, int, BOOL);

// Minimal COM smart pointer.
template <typename T>
class ComPtr {
public:
    ComPtr() : p_(NULL) {}
    ~ComPtr() { if (p_) p_->Release(); }

    T *operator->() const { return p_; }
    T *get() const { return p_; }
    T **put() { if (p_) { p_->Release(); p_ = NULL; } return &p_; }
    void **put_void() { return reinterpret_cast<void **>(put()); }
    void reset(T *p) { if (p) p->AddRef(); if (p_) p_->Release(); p_ = p; }

private:
    ComPtr(const ComPtr &);
    ComPtr &operator=(const ComPtr &);
    T *p_;
};

class ComInit {
public:
    ComInit() : hr_(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    // RPC_E_CHANGED_MODE means COM is already initialized as MTA on this thread, which also works.
    bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

private:
    HRESULT hr_;
};

// Case-insensitive comparison of paths and file names. CompareStringOrdinal (Vista+) compares the
// way the file system does; XP lacks it, so there the invariant locale is the closest available.
class PathComparer {
public:
    PathComparer()
        : ordinal_(reinterpret_cast<CompareStringOrdinalFn>(
              GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CompareStringOrdinal"))) {}

    bool Same(const wchar_t *a, const wchar_t *b) const {
        if (ordinal_) return ordinal_(a, -1, b, -1, TRUE) == CSTR_EQUAL;
        return CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, a, -1, b, -1) == CSTR_EQUAL;
    }

private:
    CompareStringOrdinalFn ordinal_;
};

// Returns the parsing name (the full path, for file system items) of an item. Free with
// CoTaskMemFree.
wchar_t *ParsingName(IShellFolder *folder, LPCITEMIDLIST pidl) {
    STRRET sr;
    wchar_t *name = NULL;
    if (FAILED(folder->GetDisplayNameOf(pidl, SHGDN_FORPARSING, &sr)) || FAILED(StrRetToStrW(&sr, pidl, &name)))
        return NULL;
    return name;
}

// Returns path in the form shared by every route to the same place: paths on mapped network drives
// become network paths ("M:\x" -> "\\server\share\x"), others stay as they are. Free with
// CoTaskMemFree.
wchar_t *CanonicalPath(const wchar_t *path) {
    UNIVERSAL_NAME_INFOW probe;
    DWORD size = sizeof(probe);
    void *buf = NULL;
    if (WNetGetUniversalNameW(path, UNIVERSAL_NAME_INFO_LEVEL, &probe, &size) == ERROR_MORE_DATA &&
        (buf = CoTaskMemAlloc(size)) != NULL &&
        WNetGetUniversalNameW(path, UNIVERSAL_NAME_INFO_LEVEL, buf, &size) == NO_ERROR)
        path = static_cast<UNIVERSAL_NAME_INFOW *>(buf)->lpUniversalName;
    wchar_t *copy = NULL;
    SHStrDupW(path, &copy);
    CoTaskMemFree(buf);
    return copy;
}

// Returns an existing path in the form the shell reports for windows and items (8.3 names expanded,
// paths over MAX_PATH shortened). Free with CoTaskMemFree. NULL if the path doesn't exist.
wchar_t *ShellPath(IShellFolder *desktop, wchar_t *path) {
    LPITEMIDLIST pidl = NULL;
    if (FAILED(desktop->ParseDisplayName(NULL, NULL, path, NULL, &pidl, NULL))) return NULL;
    wchar_t *name = ParsingName(desktop, pidl);
    CoTaskMemFree(pidl);
    return name;
}

// Gets the shell form of filePath's folder and of filePath itself (free both with CoTaskMemFree; the
// file is NULL if it doesn't exist). Returns false if the folder doesn't exist.
bool ResolvePath(IShellFolder *desktop, const wchar_t *filePath, wchar_t **folder, wchar_t **file) {
    *folder = *file = NULL;

    // Resolves relative parts and forward slashes.
    DWORD len = GetFullPathNameW(filePath, 0, NULL, NULL);
    wchar_t *full = len ? static_cast<wchar_t *>(CoTaskMemAlloc(len * sizeof(wchar_t))) : NULL;
    if (!full) return false;

    wchar_t *filePart = NULL;
    DWORD got = GetFullPathNameW(filePath, len, full, &filePart);
    if (got && got < len && filePart) {
        *file = ShellPath(desktop, full);
        *filePart = L'\0';  // leaves the folder (with a trailing backslash, which parses fine)
        *folder = ShellPath(desktop, full);
    }
    CoTaskMemFree(full);

    if (!*folder) {
        CoTaskMemFree(*file);
        *file = NULL;
        return false;
    }
    return true;
}

// Position of a window among its siblings in z-order; 0 is frontmost. For top-level windows the
// most recently activated window is in front, so a lower value means more recently used.
int ZOrderRank(HWND hwnd) {
    int rank = 0;
    while (hwnd && (hwnd = GetWindow(hwnd, GW_HWNDPREV)) != NULL) ++rank;
    return rank;
}

// Recency of an Explorer window/tab. Compared lexicographically: lower is more recent.
struct Recency {
    int  topLevelRank;  // z-order of the Explorer frame window
    bool hiddenTab;     // false for the tab currently shown in its frame (Windows 11 tabs)
    int  tabRank;       // z-order of the tab among its frame's children

    bool operator<(const Recency &o) const {
        if (topLevelRank != o.topLevelRank) return topLevelRank < o.topLevelRank;
        if (hiddenTab != o.hiddenTab) return !hiddenTab;
        return tabRank < o.tabRank;
    }
};

// If a ShellWindows entry shows folderPath, gets its IFolderView, the ID list of the folder (free
// with CoTaskMemFree), and how recently it was active. Returns false for other windows.
bool GetMatchingWindow(IDispatch *window, IShellFolder *desktop, const wchar_t *folderPath,
                       const PathComparer &comparer, IFolderView **outView, LPITEMIDLIST *outPidl,
                       Recency *outRecency) {
    ComPtr<IServiceProvider> sp;
    if (FAILED(window->QueryInterface(IID_IServiceProvider, sp.put_void()))) return false;

    ComPtr<IShellBrowser> browser;
    if (FAILED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, browser.put_void()))) return false;

    ComPtr<IShellView> shellView;
    if (FAILED(browser->QueryActiveShellView(shellView.put()))) return false;

    ComPtr<IFolderView> folderView;
    if (FAILED(shellView->QueryInterface(IID_IFolderView, folderView.put_void()))) return false;

    ComPtr<IPersistFolder2> persist;
    if (FAILED(folderView->GetFolder(IID_IPersistFolder2, persist.put_void()))) return false;
    if (FAILED(persist->GetCurFolder(outPidl)) || !*outPidl) return false;

    // Compare file system paths, so a folder reached through different routes (Desktop\Folder,
    // C:\Users\Name\Desktop\Folder, a library, a mapped drive) still matches. Folders without a
    // path (Control Panel etc.) never match.
    wchar_t *path = ParsingName(desktop, *outPidl);
    wchar_t *canonical = path ? CanonicalPath(path) : NULL;
    bool match = canonical && comparer.Same(canonical, folderPath);
    CoTaskMemFree(canonical);
    CoTaskMemFree(path);
    if (!match) {
        CoTaskMemFree(*outPidl);
        *outPidl = NULL;
        return false;
    }

    // With Windows 11 tabs every tab shares one frame window; the tab's own window is a child of it,
    // and only the selected tab's view window is visible.
    HWND tabHwnd = NULL, viewHwnd = NULL;
    browser->GetWindow(&tabHwnd);
    shellView->GetWindow(&viewHwnd);
    HWND frameHwnd = tabHwnd ? GetAncestor(tabHwnd, GA_ROOT) : NULL;
    outRecency->topLevelRank = frameHwnd ? ZOrderRank(frameHwnd) : INT_MAX;
    outRecency->hiddenTab    = !(viewHwnd && IsWindowVisible(viewHwnd));
    outRecency->tabRank      = (tabHwnd && tabHwnd != frameHwnd) ? ZOrderRank(tabHwnd) : 0;

    *outView = folderView.get();
    (*outView)->AddRef();
    return true;
}

// Reads the files (not folders) of the view in display order, and records the position of the one
// called fileName (which may be NULL). Returns NULL on failure.
ExplorerFileList *ReadItemsInViewOrder(IShellFolder *desktop, IFolderView *view, LPCITEMIDLIST folderPidl,
                                       const wchar_t *fileName, const PathComparer &comparer) {
    // Bind to the folder in this process to turn the view's item IDs into paths.
    ComPtr<IShellFolder> folder;
    if (folderPidl->mkid.cb == 0)
        folder.reset(desktop);
    else if (FAILED(desktop->BindToObject(folderPidl, NULL, IID_IShellFolder, folder.put_void())))
        return NULL;

    // Items in view order; if the view rejects that request (XP), read them by index instead.
    ComPtr<IEnumIDList> en;
    bool enumerate = SUCCEEDED(view->Items(SVGIO_ALLVIEW | kSvgioFlagViewOrder, IID_IEnumIDList, en.put_void())) &&
                     en.get();
    int itemCount = 0;
    if (!enumerate && FAILED(view->ItemCount(SVGIO_ALLVIEW, &itemCount))) return NULL;

    // Everything in the result is allocated with CoTaskMemAlloc, like the strings the shell returns,
    // so those go into the list as they are.
    ExplorerFileList *list = static_cast<ExplorerFileList *>(CoTaskMemAlloc(sizeof(ExplorerFileList)));
    if (!list) return NULL;
    list->count = 0;
    list->paths = NULL;
    list->index = -1;
    size_t capacity = 0;

    // Each Next or Item call is a round trip to Explorer's process, so Next fetches in batches.
    LPITEMIDLIST batch[128];
    ULONG batchSize = 0;

    for (int i = 0;; ++i) {
        LPITEMIDLIST child = NULL;
        ULONG slot = enumerate ? i % ARRAYSIZE(batch) : 0;
        if (enumerate) {
            if (slot == 0 && FAILED(en->Next(ARRAYSIZE(batch), batch, &batchSize))) break;
            if (slot >= batchSize) break;
            child = batch[slot];
        } else {
            if (i >= itemCount) break;
            if (FAILED(view->Item(i, &child))) continue;
        }

        // Only files. The attributes are the file system's, read from the item ID (no disk access),
        // so .zip files and shortcuts, which the shell can treat as folders, count as files.
        WIN32_FIND_DATAW data;
        bool isFile = SUCCEEDED(SHGetDataFromIDListW(folder.get(), child, SHGDFIL_FINDDATA, &data, sizeof(data))) &&
                      !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        wchar_t *path = isFile ? ParsingName(folder.get(), child) : NULL;
        CoTaskMemFree(child);
        if (!path) continue;

        if (list->count == capacity) {
            size_t grownCapacity = capacity ? capacity * 2 : 64;
            wchar_t **grown =
                static_cast<wchar_t **>(CoTaskMemRealloc(list->paths, grownCapacity * sizeof(wchar_t *)));
            if (!grown) {
                CoTaskMemFree(path);
                while (enumerate && ++slot < batchSize) CoTaskMemFree(batch[slot]);
                ExplorerOrder_Free(list);
                return NULL;
            }
            list->paths = grown;
            capacity = grownCapacity;
        }

        // The folder already matched, so comparing the name part is enough.
        if (list->index < 0 && fileName && comparer.Same(PathFindFileNameW(path), fileName))
            list->index = static_cast<ptrdiff_t>(list->count);
        list->paths[list->count++] = path;
    }
    return list;
}

ExplorerFileList *FindAndRead(IShellFolder *desktop, const wchar_t *folderPath, const wchar_t *fileName,
                              const PathComparer &comparer) {
    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, NULL, CLSCTX_ALL, IID_IShellWindows, windows.put_void())))
        return NULL;

    long windowCount = 0;
    if (FAILED(windows->get_Count(&windowCount))) return NULL;

    // If several windows/tabs show the folder, use the most recently active one.
    ComPtr<IFolderView> best;
    LPITEMIDLIST bestPidl = NULL;
    Recency bestRecency = Recency();

    // The last candidate is the desktop itself, which shows the Desktop folder but isn't in the
    // collection. It's behind every window, so a window showing the folder is preferred.
    for (long i = 0; i <= windowCount; ++i) {
        VARIANT index;  // the window's position; left empty for the desktop lookup
        VariantInit(&index);
        if (i < windowCount) {
            index.vt = VT_I4;
            index.lVal = i;
        }

        ComPtr<IDispatch> window;
        long hwnd = 0;
        HRESULT hr = i < windowCount
                         ? windows->Item(index, window.put())
                         : windows->FindWindowSW(&index, &index, kSwcDesktop, &hwnd, SWFO_NEEDDISPATCH, window.put());
        if (FAILED(hr) || !window.get()) continue;

        ComPtr<IFolderView> view;
        LPITEMIDLIST pidl = NULL;
        Recency recency;
        if (!GetMatchingWindow(window.get(), desktop, folderPath, comparer, view.put(), &pidl, &recency)) continue;

        if (!best.get() || recency < bestRecency) {
            best.reset(view.get());
            CoTaskMemFree(bestPidl);
            bestPidl = pidl;
            bestRecency = recency;
        } else {
            CoTaskMemFree(pidl);
        }
    }

    ExplorerFileList *list =
        best.get() ? ReadItemsInViewOrder(desktop, best.get(), bestPidl, fileName, comparer) : NULL;
    CoTaskMemFree(bestPidl);
    return list;
}

} // namespace

extern "C" ExplorerFileList *ExplorerOrder_GetFiles(const wchar_t *filePath) {
    if (!filePath || !*filePath) return NULL;

    ComInit com;
    if (!com.ok()) return NULL;

    ComPtr<IShellFolder> desktop;
    if (FAILED(SHGetDesktopFolder(desktop.put()))) return NULL;

    // Folders that don't exist can't be open in a window.
    wchar_t *folderPath = NULL, *shellFilePath = NULL;
    if (!ResolvePath(desktop.get(), filePath, &folderPath, &shellFilePath)) return NULL;

    PathComparer comparer;
    wchar_t *canonicalFolder = CanonicalPath(folderPath);
    ExplorerFileList *list =
        canonicalFolder ? FindAndRead(desktop.get(), canonicalFolder, shellFilePath ? PathFindFileNameW(shellFilePath) : NULL,
                                      comparer)
                        : NULL;

    CoTaskMemFree(canonicalFolder);
    CoTaskMemFree(folderPath);
    CoTaskMemFree(shellFilePath);
    return list;
}

extern "C" void ExplorerOrder_Free(ExplorerFileList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) CoTaskMemFree(list->paths[i]);
    CoTaskMemFree(list->paths);
    CoTaskMemFree(list);
}
