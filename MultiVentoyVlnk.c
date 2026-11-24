#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <winioctl.h>
#include <stdint.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#define VLNK_FILE_LEN 32768
#define VLNK_NAME_MAX 384

#define IDC_INP_DEST   101
#define IDC_BTN_DEST   102
#define IDC_BTN_RUN    103
#define IDC_LBL_STATUS 104

#pragma pack(push, 1)
typedef struct _ventoy_vlnk {
    uint8_t  guid[16];
    uint32_t crc32;
    uint32_t disk_signature;
    uint64_t part_offset;
    char     filepath[VLNK_NAME_MAX];
    uint8_t  reserved[96];
} ventoy_vlnk;
#pragma pack(pop)

static HWND g_hInpDest = NULL;
static HWND g_hLblStatus = NULL;
static uint32_t g_crc32c_table[256];
static uint8_t g_vlnkBuf[VLNK_FILE_LEN];
static WCHAR g_fileBuf[65536];

/* ============================================================================
 * CRC32C (Castagnoli 0x82F63B78 - khop 100% crc32.c cua Ventoy)
 * ============================================================================ */
static void InitCRC32CTable(void) {
    uint32_t i, j, crc;
    for (i = 0; i < 256; i++) {
        crc = i;
        for (j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ 0x82F63B78U) : (crc >> 1);
        }
        g_crc32c_table[i] = crc;
    }
}

static uint32_t CalcCRC32C(const void* buf, size_t size) {
    const uint8_t* data = (const uint8_t*)buf;
    uint32_t crc = 0xFFFFFFFFU;
    size_t i;
    for (i = 0; i < size; i++) {
        crc = (crc >> 8) ^ g_crc32c_table[(crc ^ data[i]) & 0xFFU];
    }
    return crc ^ 0xFFFFFFFFU;
}

/* ============================================================================
 * WIN32 API: LAY DISK NUMBER, PARTITION OFFSET & DISK SIGNATURE
 * ============================================================================ */
static BOOL GetVolumeDiskInfo(WCHAR driveLetter, DWORD* diskNum, uint64_t* partOffset) {
    WCHAR volPath[16];
    HANDLE hVol;
    VOLUME_DISK_EXTENTS extents;
    DWORD bytesRet = 0;
    BOOL ok;

    wsprintfW(volPath, L"\\\\.\\%c:", driveLetter);
    hVol = CreateFileW(volPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) return FALSE;

    ZeroMemory(&extents, sizeof(extents));
    ok = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         NULL, 0, &extents, sizeof(extents), &bytesRet, NULL);
    CloseHandle(hVol);

    if (!ok || extents.NumberOfDiskExtents < 1) return FALSE;
    *diskNum = extents.Extents[0].DiskNumber;
    *partOffset = (uint64_t)extents.Extents[0].StartingOffset.QuadPart;
    return TRUE;
}

static BOOL GetPhysicalDiskSignature(DWORD diskNum, uint32_t* diskSig) {
    WCHAR diskPath[64];
    HANDLE hDisk;
    uint8_t sector[512];
    DWORD bytesRead = 0;
    BOOL ok;

    wsprintfW(diskPath, L"\\\\.\\PhysicalDrive%lu", (unsigned long)diskNum);
    hDisk = CreateFileW(diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (hDisk == INVALID_HANDLE_VALUE) return FALSE;

    ZeroMemory(sector, sizeof(sector));
    ok = ReadFile(hDisk, sector, 512, &bytesRead, NULL);
    CloseHandle(hDisk);

    if (!ok || bytesRead < 512) return FALSE;
    memcpy(diskSig, sector + 0x1B8, 4);
    return TRUE;
}

/* ============================================================================
 * TU DONG TIM PHAN VUNG DU LIEU VENTOY
 * ============================================================================ */
static BOOL ContainsWordNoCase(const WCHAR* str, const WCHAR* sub) {
    WCHAR buf[MAX_PATH];
    int lenSub = lstrlenW(sub);
    int lenStr, i, j;

    lstrcpynW(buf, str, MAX_PATH);
    CharUpperW(buf);
    lenStr = lstrlenW(buf);
    for (i = 0; i <= lenStr - lenSub; i++) {
        for (j = 0; j < lenSub; j++) {
            if (buf[i + j] != sub[j]) break;
        }
        if (j == lenSub) return TRUE;
    }
    return FALSE;
}

static void FindVentoyDataPartition(WCHAR* outRoot, int maxChars) {
    DWORD drives = GetLogicalDrives();
    DWORD ventoyDisk = (DWORD)-1;
    WCHAR fallbackDrive[4] = { 0 };
    int i;

    outRoot[0] = 0;

    for (i = 0; i < 26; i++) {
        WCHAR letter, root[4], volName[MAX_PATH];
        if (!(drives & (1U << i))) continue;
        letter = (WCHAR)(L'A' + i);
        root[0] = letter; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
        volName[0] = 0;

        if (GetVolumeInformationW(root, volName, MAX_PATH, NULL, NULL, NULL, NULL, 0)) {
            if (lstrcmpiW(volName, L"VTOYEFI") == 0) {
                DWORD dNum = 0;
                uint64_t pOff = 0;
                if (GetVolumeDiskInfo(letter, &dNum, &pOff)) {
                    ventoyDisk = dNum;
                }
            } else if (fallbackDrive[0] == 0 && ContainsWordNoCase(volName, L"VENTOY")) {
                lstrcpynW(fallbackDrive, root, 4);
            }
        }
    }

    if (ventoyDisk != (DWORD)-1) {
        for (i = 0; i < 26; i++) {
            WCHAR letter, root[4], volName[MAX_PATH];
            DWORD dNum = 0;
            uint64_t pOff = 0;
            if (!(drives & (1U << i))) continue;
            letter = (WCHAR)(L'A' + i);
            root[0] = letter; root[1] = L':'; root[2] = L'\\'; root[3] = 0;
            volName[0] = 0;

            if (GetVolumeInformationW(root, volName, MAX_PATH, NULL, NULL, NULL, NULL, 0)) {
                if (lstrcmpiW(volName, L"VTOYEFI") == 0) continue;
                if (GetVolumeDiskInfo(letter, &dNum, &pOff) && dNum == ventoyDisk) {
                    lstrcpynW(outRoot, root, maxChars);
                    return;
                }
            }
        }
    }

    if (fallbackDrive[0] != 0) {
        lstrcpynW(outRoot, fallbackDrive, maxChars);
    }
}

/* ============================================================================
 * TAO FILE .VLNK 32 KB TRUC TIEP
 * ============================================================================ */
static BOOL IsSupportedExt(const WCHAR* ext) {
    static const WCHAR* exts[] = { L".iso", L".wim", L".vhd", L".vhdx", L".img", L".efi", L".vtoy", L".dat" };
    int i;
    for (i = 0; i < 8; i++) {
        if (lstrcmpiW(ext, exts[i]) == 0) return TRUE;
    }
    return FALSE;
}

static int CreateNativeVlink(const WCHAR* srcPath, const WCHAR* outDir, WCHAR* errMsg, int errMax) {
    static const uint8_t ventoyGuid[16] = {
        0x20, 0x20, 0x77, 0x77, 0x77, 0x2E, 0x76, 0x65,
        0x6E, 0x74, 0x6F, 0x79, 0x2E, 0x6E, 0x65, 0x74
    };
    int len = lstrlenW(srcPath);
    int dotPos = -1, slashPos = -1, i;
    WCHAR extLower[16];
    WCHAR baseName[MAX_PATH];
    WCHAR relPathW[MAX_PATH];
    WCHAR outFile[MAX_PATH];
    char utf8Path[VLNK_NAME_MAX];
    DWORD diskNum = 0, written = 0;
    uint64_t partOffset = 0;
    uint32_t diskSig = 0;
    int utf8Len;
    ventoy_vlnk* vlnk;
    HANDLE hFile;
    BOOL ok;

    if (len < 5 || srcPath[1] != L':') {
        lstrcpynW(errMsg, L"Đường dẫn nguồn không hợp lệ.", errMax);
        return 0;
    }

    for (i = len - 1; i >= 0; i--) {
        if (srcPath[i] == L'.' && dotPos == -1) dotPos = i;
        if (srcPath[i] == L'\\' || srcPath[i] == L'/') {
            slashPos = i;
            break;
        }
    }
    if (dotPos == -1 || dotPos <= slashPos) return -1;

    lstrcpynW(extLower, srcPath + dotPos, 16);
    CharLowerW(extLower);
    if (!IsSupportedExt(extLower)) return -1;

    {
        int nameStart = (slashPos >= 0) ? (slashPos + 1) : 0;
        int nameLen = dotPos - nameStart;
        if (nameLen <= 0 || nameLen >= MAX_PATH) return 0;
        memcpy(baseName, srcPath + nameStart, nameLen * sizeof(WCHAR));
        baseName[nameLen] = 0;
    }

    if (!GetVolumeDiskInfo(srcPath[0], &diskNum, &partOffset)) {
        lstrcpynW(errMsg, L"Không đọc được Volume Disk Extents (cần quyền Admin).", errMax);
        return 0;
    }
    if (!GetPhysicalDiskSignature(diskNum, &diskSig)) {
        lstrcpynW(errMsg, L"Không đọc được Disk Signature của PhysicalDrive.", errMax);
        return 0;
    }

    lstrcpynW(relPathW, srcPath + 2, MAX_PATH);
    for (i = 0; relPathW[i]; i++) {
        if (relPathW[i] == L'\\') relPathW[i] = L'/';
    }

    ZeroMemory(utf8Path, sizeof(utf8Path));
    utf8Len = WideCharToMultiByte(CP_UTF8, 0, relPathW, -1, utf8Path, VLNK_NAME_MAX, NULL, NULL);
    if (utf8Len <= 0 || utf8Len >= VLNK_NAME_MAX) {
        lstrcpynW(errMsg, L"Đường dẫn quá dài (>= 384 bytes UTF-8).", errMax);
        return 0;
    }

    ZeroMemory(g_vlnkBuf, VLNK_FILE_LEN);
    vlnk = (ventoy_vlnk*)g_vlnkBuf;
    memcpy(vlnk->guid, ventoyGuid, 16);
    vlnk->crc32 = 0;
    vlnk->disk_signature = diskSig;
    vlnk->part_offset = partOffset;
    memcpy(vlnk->filepath, utf8Path, utf8Len);
    vlnk->crc32 = CalcCRC32C(vlnk, sizeof(ventoy_vlnk));

    lstrcpynW(outFile, outDir, MAX_PATH);
    len = lstrlenW(outFile);
    if (len > 0 && outFile[len - 1] != L'\\' && len + 1 < MAX_PATH) {
        outFile[len] = L'\\';
        outFile[len + 1] = 0;
    }
    if (lstrlenW(outFile) + lstrlenW(baseName) + lstrlenW(extLower) + 6 >= MAX_PATH) {
        lstrcpynW(errMsg, L"Đường dẫn file đích quá dài.", errMax);
        return 0;
    }
    lstrcatW(outFile, baseName);
    lstrcatW(outFile, L".vlnk");
    lstrcatW(outFile, extLower);

    hFile = CreateFileW(outFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        lstrcpynW(errMsg, L"Không thể tạo file đích trên phân vùng Ventoy.", errMax);
        return 0;
    }

    ok = WriteFile(hFile, g_vlnkBuf, VLNK_FILE_LEN, &written, NULL);
    CloseHandle(hFile);

    if (!ok || written != VLNK_FILE_LEN) {
        lstrcpynW(errMsg, L"Ghi file không đủ 32 KB.", errMax);
        return 0;
    }
    return 1;
}

/* ============================================================================
 * XU LY CHON NHIEU FILE & TAO HANG LOAT
 * ============================================================================ */
static void RunBatchVlink(HWND hWnd) {
    WCHAR destDir[MAX_PATH] = { 0 };
    WCHAR lastErr[256] = { 0 };
    WCHAR status[512];
    WCHAR fullPath[MAX_PATH];
    OPENFILENAMEW ofn;
    const WCHAR* p;
    const WCHAR* first;
    int total = 0, idx = 0, success = 0, failed = 0, skipped = 0;

    GetWindowTextW(g_hInpDest, destDir, MAX_PATH);
    if (destDir[0] == 0 || GetFileAttributesW(destDir) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hWnd, L"Vui lòng chọn phân vùng hoặc thư mục đích hợp lệ trên Ventoy.", L"Thiếu thư mục đích", MB_ICONERROR);
        return;
    }

    ZeroMemory(g_fileBuf, sizeof(g_fileBuf));
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Boot Images (*.iso;*.wim;*.vhd;*.vhdx;*.img;*.efi)\0*.iso;*.wim;*.vhd;*.vhdx;*.img;*.efi\0"
                      L"ISO Images (*.iso)\0*.iso\0"
                      L"WIM Images (*.wim)\0*.wim\0"
                      L"VHD/VHDX (*.vhd;*.vhdx)\0*.vhd;*.vhdx\0"
                      L"IMG/EFI (*.img;*.efi)\0*.img;*.efi\0";
    ofn.lpstrFile = g_fileBuf;
    ofn.nMaxFile = 65536;
    ofn.lpstrTitle = L"Chọn nhiều file ISO, WIM, VHD, IMG, EFI để tạo Vlink";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    first = g_fileBuf;
    p = first + lstrlenW(first) + 1;

    if (*p == 0) {
        int res;
        SetWindowTextW(g_hLblStatus, L"Đang tạo vlink (1/1)...");
        res = CreateNativeVlink(first, destDir, lastErr, 256);
        if (res == 1) success++;
        else if (res == -1) skipped++;
        else failed++;
    } else {
        const WCHAR* scan = p;
        while (*scan != 0) {
            total++;
            scan += lstrlenW(scan) + 1;
        }
        while (*p != 0) {
            int dirLen = lstrlenW(first);
            int res;
            idx++;
            wsprintfW(status, L"Đang tạo vlink (%d/%d)...", idx, total);
            SetWindowTextW(g_hLblStatus, status);

            lstrcpynW(fullPath, first, MAX_PATH);
            if (dirLen > 0 && fullPath[dirLen - 1] != L'\\' && dirLen + 1 < MAX_PATH) {
                fullPath[dirLen] = L'\\';
                fullPath[dirLen + 1] = 0;
            }
            if (lstrlenW(fullPath) + lstrlenW(p) < MAX_PATH) {
                lstrcatW(fullPath, p);
                res = CreateNativeVlink(fullPath, destDir, lastErr, 256);
            } else {
                lstrcpynW(lastErr, L"Đường dẫn nguồn quá dài.", 256);
                res = 0;
            }
            if (res == 1) success++;
            else if (res == -1) skipped++;
            else failed++;

            p += lstrlenW(p) + 1;
        }
    }

    if (failed > 0 && lastErr[0] != 0) {
        wsprintfW(status, L"Hoàn tất! Đích: %s\r\nThành công: %d | Bỏ qua: %d | Thất bại: %d\r\nLỗi gần nhất: %s",
                  destDir, success, skipped, failed, lastErr);
    } else {
        wsprintfW(status, L"Hoàn tất! Đích: %s\r\nThành công: %d | Bỏ qua: %d | Thất bại: %d",
                  destDir, success, skipped, failed);
    }

    SetWindowTextW(g_hLblStatus, status);
    MessageBoxW(hWnd, status, L"Kết quả tạo Vlink", MB_ICONINFORMATION);
}

static void BrowseDestFolder(HWND hWnd) {
    BROWSEINFOW bi;
    LPITEMIDLIST pidl;
    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = hWnd;
    bi.lpszTitle = L"Chọn phân vùng / thư mục đích trên Ventoy:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        WCHAR path[MAX_PATH] = { 0 };
        if (SHGetPathFromIDListW(pidl, path)) {
            int len = lstrlenW(path);
            if (len > 0 && path[len - 1] != L'\\' && len + 1 < MAX_PATH) {
                path[len] = L'\\';
                path[len + 1] = 0;
            }
            SetWindowTextW(g_hInpDest, path);
        }
        CoTaskMemFree(pidl);
    }
}

/* ============================================================================
 * CUA SO WIN32 GUI
 * ============================================================================ */
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            WCHAR detected[MAX_PATH];
            HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT hFontBold = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HWND hLbl1, hBtnDest, hBtnRun;

            FindVentoyDataPartition(detected, MAX_PATH);
            hLbl1 = CreateWindowW(L"STATIC", L"1. Phân vùng / Thư mục Ventoy:",
                                  WS_VISIBLE | WS_CHILD, 15, 22, 195, 22, hWnd, NULL, NULL, NULL);
            g_hInpDest = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", detected,
                                         WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 215, 19, 245, 26, hWnd, (HMENU)IDC_INP_DEST, NULL, NULL);
            hBtnDest = CreateWindowW(L"BUTTON", L"Chọn...",
                                     WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 470, 18, 75, 28, hWnd, (HMENU)IDC_BTN_DEST, NULL, NULL);
            hBtnRun = CreateWindowW(L"BUTTON", L"2. Chọn nhiều file (ISO, WIM, VHD, IMG, EFI) && Tạo Vlink",
                                    WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 15, 62, 530, 42, hWnd, (HMENU)IDC_BTN_RUN, NULL, NULL);
            g_hLblStatus = CreateWindowW(L"STATIC", L"Sẵn sàng. Bản Pure C siêu nhẹ ghi trực tiếp file .vlnk 32 KB chuẩn Ventoy.",
                                         WS_VISIBLE | WS_CHILD, 15, 118, 530, 80, hWnd, (HMENU)IDC_LBL_STATUS, NULL, NULL);

            SendMessageW(hLbl1, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(g_hInpDest, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hBtnDest, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessageW(hBtnRun, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            SendMessageW(g_hLblStatus, WM_SETFONT, (WPARAM)hFont, TRUE);
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_BTN_DEST) BrowseDestFolder(hWnd);
            else if (LOWORD(wParam) == IDC_BTN_RUN) RunBatchVlink(hWnd);
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            break;
        default:
            return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow) {
    WNDCLASSW wc;
    HWND hWnd;
    MSG msg;

    (void)hPrevInstance;
    (void)lpCmdLine;

    CoInitialize(NULL);
    InitCRC32CTable();

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"MultiVentoyVlnkPureCClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    hWnd = CreateWindowExW(0, wc.lpszClassName, L"Multi Ventoy Vlink Creator (Tiny C)",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 575, 245, NULL, NULL, hInstance, NULL);
    if (!hWnd) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
