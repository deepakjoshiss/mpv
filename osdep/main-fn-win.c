#include <windows.h>

#ifndef BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE
#define BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE (0x0001)
#endif

#include "common/common.h"
#include "osdep/io.h"
#include "osdep/terminal.h"
#include "osdep/main-fn.h"
#include "misc/bstr.h"

static bool is_valid_handle(HANDLE h)
{
    return h != INVALID_HANDLE_VALUE && h != NULL &&
           GetFileType(h) != FILE_TYPE_UNKNOWN;
}

static bool has_redirected_stdio(void)
{
    return is_valid_handle(GetStdHandle(STD_INPUT_HANDLE)) ||
           is_valid_handle(GetStdHandle(STD_OUTPUT_HANDLE)) ||
           is_valid_handle(GetStdHandle(STD_ERROR_HANDLE));
}

static void microsoft_nonsense(void)
{
    // stop Windows from showing all kinds of annoying error dialogs
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // Enable heap corruption detection
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    WINBOOL (WINAPI *pSetSearchPathMode)(DWORD Flags) =
        (WINBOOL (WINAPI *)(DWORD))GetProcAddress(kernel32, "SetSearchPathMode");

    // Always use safe search paths for DLLs and other files, ie. never use the
    // current directory
    SetDllDirectoryW(L"");
    if (pSetSearchPathMode)
        pSetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE);
}

int check_open_in_window(int argv_len, char * argv[], bool processExist, int tries)
{
    HWND hWnd = FindWindowEx(0, 0, L"DJmpv", 0);

    if (hWnd == 0)
    {
        fprintf(stdout, ">>> cannot find a window  - process %d\n", processExist);
        if(processExist && tries < 6) {
            Sleep(500);
            tries++;
            fprintf(stdout, ">>> retrying to find a window try - %d\n", tries);
            return check_open_in_window(argv_len, argv, processExist, tries);
        }
        return 1;
    }
    else
    {
        fprintf(stdout, "====== yes found a window %d  \n", strlen(argv[1]));

        int fileLen = 0;
        int totalSize = 0;
        
        for (int i = 1; i < argv_len; i++) // skips program name
        {   
            if (bstr_startswith0(bstr0(argv[i]), "-"))
            {
                fprintf(stdout, ">>>>>>> got option %d = %s >>>>>> \n", i, argv[i]);
            }
            else
            {
                totalSize = sizeof(char) * (strlen(argv[i]) + 1);
                fprintf(stdout, "====== sending data %d and %d and %s \n", i, totalSize, argv[i]);
                HGLOBAL hG = GlobalAlloc(GMEM_FIXED, totalSize);
                char *str = (char *)GlobalLock(hG);
                char *str1 = argv[i];
                int j = 0;

                while (str1[j] != '\0')
                {
                    str[j] = str1[j];
                    // fprintf(stdout, "====== next char is %c \n", str1[j]);
                    j++;
                }
                str[j] = '\0';
    
                // int k = 0;
                // while (k <= j + 10)
                // {
                //     fprintf(stdout, "====== again next char is %c \n", str[k]);
                //     k++;
                // }

                COPYDATASTRUCT cds;
                cds.dwData = fileLen + tries; // can be anything
                cds.cbData = totalSize;
                cds.lpData = str;

                SendMessage(hWnd, WM_COPYDATA, 0, (LPARAM)(LPVOID)&cds);
                fileLen++;
                GlobalFree(hG);
            }
        }
        return 0;
    }
}

int main(int argc_, char **argv_)
{
    char buffer[200];
    snprintf(buffer, sizeof(buffer), "C:/Data/projects/mpv/bin/log-%ld.txt", GetCurrentProcessId());
    freopen(buffer, "w+", stdout);
    
    fprintf(stdout, "====== Starting process %ld \n", GetCurrentProcessId());
    HANDLE hMutex = CreateMutex(0, 0, L"DJmpv.App");
    bool processExist = false;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        processExist = true;
        fprintf(stdout, "====== Process already exist\n");
    }
    else
    {
        fprintf(stdout, ">>>>> Process does not exist \n");
    }

    microsoft_nonsense();

    // If started from the console wrapper (see osdep/win32-console-wrapper.c),
    // attach to the console and set up the standard IO handles
    bool has_console = terminal_try_attach();

    // If mpv is started from Explorer, the Run dialog or the Start Menu, it
    // will have no console and no standard IO handles. In this case, the user
    // is expecting mpv to show some UI, so enable the pseudo-GUI profile.
    bool gui = !has_console && !has_redirected_stdio();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int argv_len = 0;
    char **argv_u8 = NULL;
    
    bool new_window = !processExist;
    // Build mpv's UTF-8 argv, and add the pseudo-GUI profile if necessary
    if (argc > 0 && argv[0])
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len, mp_to_utf8(argv_u8, argv[0]));
    if (gui)
    {
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len,
                         "--player-operation-mode=pseudo-gui");
    }

    for (int i = 1; i < argc; i++)
    {
        MP_TARRAY_APPEND(NULL, argv_u8, argv_len, mp_to_utf8(argv_u8, argv[i]));
        fprintf(stdout, "====== got argument %d = %s ===== \n", i, argv_u8[i]);
        if (!new_window && (strcmp("--window-new=yes", argv_u8[i]) == 0 || strcmp("-window-new=yes", argv_u8[i]) == 0))
        {
            new_window = true;
            fprintf(stdout, "\n ====== will open in new window========\n");
        }
    }
    MP_TARRAY_APPEND(NULL, argv_u8, argv_len, NULL);
    
    if (!new_window && check_open_in_window(argv_len - 1, argv_u8, processExist, 0) == 0)
    {
        ReleaseMutex(hMutex);
        return 0;
    }
    int ret = mpv_main(argv_len - 1, argv_u8);

    talloc_free(argv_u8);
    //ReleaseMutex(hMutex);
    fprintf(stdout, "====== Process over \n");
    return ret;
}
