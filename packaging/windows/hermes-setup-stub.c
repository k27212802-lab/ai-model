/* Hermes-Agent-Setup.exe
 * A console stub. The zip of the Hermes source is appended:
 *   [this exe][zip bytes][uint64 little-endian zip size][4 bytes "HMSP"]
 * The zip is written to G:\hermes when that disk is ready and has room,
 * otherwise under %LOCALAPPDATA%\hermes, then scripts\install.ps1 -Payload runs.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void say(const wchar_t *msg) {
    DWORD wrote = 0;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE &&
        WriteConsoleW(out, msg, (DWORD)wcslen(msg), &wrote, NULL)) {
        WriteConsoleW(out, L"\r\n", 2, &wrote, NULL);
        return;
    }
    fwprintf(stderr, L"%ls\n", msg);
}

static void fail(const wchar_t *msg) {
    say(msg);
    MessageBoxW(NULL, msg, L"Hermes Agent", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static int drive_ok(const wchar_t *root) {
    UINT kind = GetDriveTypeW(root);
    if (kind != DRIVE_FIXED && kind != DRIVE_REMOVABLE &&
        kind != DRIVE_REMOTE && kind != DRIVE_RAMDISK) {
        return 0;
    }
    ULARGE_INTEGER free_bytes;
    if (!GetDiskFreeSpaceExW(root, &free_bytes, NULL, NULL)) {
        return 0;
    }
    /* The packed tree plus Python and Git needs a few gigabytes. */
    return free_bytes.QuadPart >= (4ull << 30);
}

static int ensure_dir(const wchar_t *path) {
    if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 1;
    }
    return 0;
}

static int copy_range(HANDLE src, unsigned long long offset, unsigned long long size, const wchar_t *dest) {
    HANDLE out = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER pos;
    unsigned char *buf;
    unsigned long long left;
    if (out == INVALID_HANDLE_VALUE) {
        return 0;
    }
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(src, pos, NULL, FILE_BEGIN)) {
        CloseHandle(out);
        return 0;
    }
    buf = (unsigned char *)malloc(1024 * 1024);
    if (!buf) {
        CloseHandle(out);
        return 0;
    }
    left = size;
    while (left) {
        DWORD chunk = left > 1024 * 1024 ? 1024 * 1024 : (DWORD)left;
        DWORD got = 0;
        DWORD wrote = 0;
        if (!ReadFile(src, buf, chunk, &got, NULL) || got != chunk) {
            free(buf);
            CloseHandle(out);
            return 0;
        }
        if (!WriteFile(out, buf, got, &wrote, NULL) || wrote != got) {
            free(buf);
            CloseHandle(out);
            return 0;
        }
        left -= got;
    }
    free(buf);
    CloseHandle(out);
    return 1;
}

static int run_command(wchar_t *cmdline) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int main(void) {
    wchar_t exe[32768];
    wchar_t home[2048];
    wchar_t install[2200];
    wchar_t zip_path[2300];
    wchar_t cmd[8192];
    wchar_t sysroot[MAX_PATH];
    HANDLE self;
    LARGE_INTEGER file_size;
    unsigned long long zip_size = 0;
    unsigned char tail[12];
    DWORD got = 0;
    int rc;

    SetConsoleTitleW(L"Hermes Agent");
    if (!GetModuleFileNameW(NULL, exe, 32768)) {
        fail(L"Не удалось узнать путь к этому exe.");
    }
    self = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (self == INVALID_HANDLE_VALUE) {
        fail(L"Не удалось открыть этот exe.");
    }
    if (!GetFileSizeEx(self, &file_size) || file_size.QuadPart < 12) {
        CloseHandle(self);
        fail(L"Файл exe повреждён: нет архива Hermes.");
    }
    {
        LARGE_INTEGER pos;
        pos.QuadPart = file_size.QuadPart - 12;
        if (!SetFilePointerEx(self, pos, NULL, FILE_BEGIN) ||
            !ReadFile(self, tail, 12, &got, NULL) || got != 12) {
            CloseHandle(self);
            fail(L"Не удалось прочитать архив внутри exe.");
        }
    }
    if (memcmp(tail + 8, "HMSP", 4) != 0) {
        CloseHandle(self);
        fail(L"В этом exe нет архива Hermes Agent.");
    }
    memcpy(&zip_size, tail, 8);
    if (zip_size == 0 || zip_size > (unsigned long long)file_size.QuadPart - 12) {
        CloseHandle(self);
        fail(L"Архив внутри exe повреждён.");
    }

    if (drive_ok(L"G:\\")) {
        wcscpy(home, L"G:\\hermes");
        say(L"Диск G найден. Hermes Agent распакуется в G:\\hermes и не займёт диск C.");
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", home, 800);
        if (n == 0 || n >= 800) {
            CloseHandle(self);
            fail(L"Диск G недоступен, и папка LocalAppData не найдена.");
        }
        wcscat(home, L"\\hermes");
        say(L"Диск G недоступен. Hermes Agent распакуется в профиль на диске C.");
    }
    wcscpy(install, home);
    wcscat(install, L"\\hermes-agent");
    wcscpy(zip_path, home);
    wcscat(zip_path, L"\\hermes-agent.zip");

    if (!ensure_dir(home) || !ensure_dir(install)) {
        CloseHandle(self);
        fail(L"Не удалось создать папку установки.");
    }
    say(L"Копирование архива на выбранный диск...");
    if (!copy_range(self, (unsigned long long)file_size.QuadPart - 12 - zip_size, zip_size, zip_path)) {
        CloseHandle(self);
        fail(L"Не удалось записать архив. Проверьте свободное место.");
    }
    CloseHandle(self);

    if (!GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH)) {
        wcscpy(sysroot, L"C:\\Windows");
    }
    wcscpy(cmd, L"\"");
    wcscat(cmd, sysroot);
    wcscat(cmd, L"\\System32\\tar.exe\" -xf \"");
    wcscat(cmd, zip_path);
    wcscat(cmd, L"\" -C \"");
    wcscat(cmd, install);
    wcscat(cmd, L"\"");
    say(L"Распаковка Hermes Agent...");
    rc = run_command(cmd);
    if (rc != 0) {
        fail(L"Распаковка не удалась. Нужен tar.exe из Windows 10 или новее.");
    }
    DeleteFileW(zip_path);

    wcscpy(cmd, L"\"");
    wcscat(cmd, sysroot);
    wcscat(cmd, L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -File \"");
    wcscat(cmd, install);
    wcscat(cmd, L"\\scripts\\install.ps1\" -Payload -NonInteractive -HermesHome \"");
    wcscat(cmd, home);
    wcscat(cmd, L"\" -InstallDir \"");
    wcscat(cmd, install);
    wcscat(cmd, L"\"");
    say(L"Установка. Исходники уже в exe; Python и Git докачиваются в ту же папку.");
    rc = run_command(cmd);
    if (rc != 0) {
        fail(L"Установка Hermes Agent не завершилась. Смотрите текст выше.");
    }
    say(L"Готово. Откройте новое окно PowerShell и выполните: hermes");
    return 0;
}
