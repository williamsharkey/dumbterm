/*
 * dumbterm_hook.dll — minimal isTTY faker for pipes.
 *
 * Injected into child (node.exe). Makes GetConsoleMode succeed and
 * GetFileType return FILE_TYPE_CHAR for stdout/stderr/stdin ONLY.
 * Other handles pass through to the real functions.
 *
 * Build: gcc -shared -O2 -o dumbterm_hook.dll hook.c -lkernel32
 */
#include <windows.h>
#include <string.h>

/* saved real functions + handles */
static BOOL  (WINAPI *real_GetConsoleMode)(HANDLE, LPDWORD);
static DWORD (WINAPI *real_GetFileType)(HANDLE);
static BOOL  (WINAPI *real_GetCSBI)(HANDLE, PCONSOLE_SCREEN_BUFFER_INFO);
static HANDLE h_out, h_err, h_in;

static int is_stdio(HANDLE h) { return h == h_out || h == h_err || h == h_in; }

/* ── hooks ─────────────────────────────────────────────────────── */

static BOOL WINAPI hook_GetConsoleMode(HANDLE h, LPDWORD mode) {
    if (is_stdio(h)) {
        *mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | 0x0004;
        return TRUE;
    }
    return real_GetConsoleMode ? real_GetConsoleMode(h, mode) : FALSE;
}

static DWORD WINAPI hook_GetFileType(HANDLE h) {
    if (is_stdio(h)) return FILE_TYPE_CHAR;
    return real_GetFileType ? real_GetFileType(h) : FILE_TYPE_UNKNOWN;
}

static BOOL WINAPI hook_GetCSBI(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info) {
    if (is_stdio(h)) {
        memset(info, 0, sizeof(*info));
        info->dwSize.X = 120; info->dwSize.Y = 40;
        info->srWindow.Right = 119; info->srWindow.Bottom = 39;
        info->dwMaximumWindowSize.X = 120; info->dwMaximumWindowSize.Y = 40;
        info->wAttributes = 7;
        return TRUE;
    }
    return real_GetCSBI ? real_GetCSBI(h, info) : FALSE;
}

/* ── IAT patcher ───────────────────────────────────────────────── */

static void patch(HMODULE mod, const char *func, void *hook, void **orig) {
    if (!mod) return;
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;

    IMAGE_IMPORT_DESCRIPTOR *imp;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
        const char *dll = (const char *)(base + imp->Name);
        /* match kernel32.dll (case-insensitive first char check) */
        if ((dll[0]|0x20) != 'k') continue;

        IMAGE_THUNK_DATA *ot = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; ot->u1.AddressOfData; ot++, ft++) {
            if (ot->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME *n = (IMAGE_IMPORT_BY_NAME *)(base + ot->u1.AddressOfData);
            if (strcmp((const char *)n->Name, func) != 0) continue;

            if (orig && !*orig) *orig = (void *)ft->u1.Function;
            DWORD old;
            VirtualProtect(&ft->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            ft->u1.Function = (DWORD_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void*), old, &old);
            return;
        }
    }
}

/* ── entry ─────────────────────────────────────────────────────── */

static void hlog(const char *msg) {
    HANDLE f = CreateFileA("C:\\workspace\\dumbterm-test\\hook.log",
        FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w; WriteFile(f, msg, strlen(msg), &w, NULL);
        WriteFile(f, "\r\n", 2, &w, NULL);
        CloseHandle(f);
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        hlog("HOOK: DllMain start");

        h_out = GetStdHandle(STD_OUTPUT_HANDLE);
        h_err = GetStdHandle(STD_ERROR_HANDLE);
        h_in  = GetStdHandle(STD_INPUT_HANDLE);
        hlog("HOOK: got stdio handles");

        HMODULE exe = GetModuleHandleA(NULL);
        hlog("HOOK: patching exe IAT");
        patch(exe, "GetConsoleMode", hook_GetConsoleMode, (void**)&real_GetConsoleMode);
        hlog("HOOK: patched GetConsoleMode");
        patch(exe, "GetFileType", hook_GetFileType, (void**)&real_GetFileType);
        hlog("HOOK: patched GetFileType");
        patch(exe, "GetConsoleScreenBufferInfo", hook_GetCSBI, (void**)&real_GetCSBI);
        hlog("HOOK: patched GetCSBI");

        HMODULE ucrt = GetModuleHandleA("ucrtbase.dll");
        if (ucrt) {
            hlog("HOOK: patching ucrtbase");
            patch(ucrt, "GetConsoleMode", hook_GetConsoleMode, (void**)&real_GetConsoleMode);
            patch(ucrt, "GetFileType", hook_GetFileType, (void**)&real_GetFileType);
            patch(ucrt, "GetConsoleScreenBufferInfo", hook_GetCSBI, (void**)&real_GetCSBI);
            hlog("HOOK: ucrtbase patched");
        } else {
            hlog("HOOK: ucrtbase not loaded");
        }

        hlog("HOOK: DllMain done");
    }
    return TRUE;
}
