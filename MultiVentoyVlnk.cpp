#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <cwctype>
#include <string>
#include <vector>
#include <algorithm>

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
struct ventoy_vlnk {
    uint8_t  guid[16];
    uint32_t crc32;
    uint32_t disk_signature;
    uint64_t part_offset;
    char     filepath[VLNK_NAME_MAX];
    uint8_t  reserved[96];
};
#pragma pack(pop)

static HWND g_hInpDest = NULL;
static HWND g_hLblStatus = NULL;
static uint32_t g_crc32c_table[256];

// ============================================================================
// CRC32C (Castagnoli 0x82F63B78 - khop 100% crc32.c cua Ventoy)
// ============================================================================
static void InitCRC32CTable() {
    const uint32_t poly = 0x82F63B78;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? ((crc >> 1) ^ poly) : (crc >> 1);
        }
        g_crc32c_table[i] = crc;
    }
}

static uint32_t CalcCRC32C(const void* buf, size_t size) {
    const uint8_t* data = (const uint8_t*)buf;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 8) ^ g_crc32c_table[(crc ^ data[i]) & 0xFF];
    }
    return crc ^ 0xFFFFFFFF;
}

// ============================================================================
// WIN32 API: LAY DISK NUMBER, PARTITION OFFSET & DISK SIGNATURE
// ============================================================================
static bool GetVolumeDiskInfo(wchar_t driveLetter, DWORD& diskNum, uint64_t& partOffset) {
    wchar_t volPath[16];
    swprintf(volPath, 16, L"\\\\.\\%c:", driveLetter);

    HANDLE hVol = CreateFileW(volPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
    if (hVol == INVALID_HANDLE_VALUE) return false;

    VOLUME_DISK_EXTENTS extents = { 0 };
    DWORD bytesRet = 0;
    BOOL ok = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                              NULL, 0, &extents, sizeof(extents), &bytesRet, NULL);
    CloseHandle(hVol);

    if (!ok || extents.NumberOfDiskExtents < 1) return false;
    diskNum = extents.Extents[0].DiskNumber;
    partOffset = (uint64_t)extents.Extents[0].StartingOffset.QuadPart;
    return true;
}

static bool GetPhysicalDiskSignature(DWORD diskNum, uint32_t& diskSig) {
    wchar_t diskPath[64];
    swprintf(diskPath, 64, L"\\\\.\\PhysicalDrive%lu", diskNum);

    HANDLE hDisk = CreateFileW(diskPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
    if (hDisk == INVALID_HANDLE_VALUE) return false;

    uint8_t sector[512] = { 0 };
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hDisk, sector, 512, &bytesRead, NULL);
    CloseHandle(hDisk);

    if (!ok || bytesRead < 512) return false;
    memcpy(&diskSig, sector + 0x1B8, 4);
    return true;
}

// ============================================================================
// TU DONG TIM PHAN VUNG DU LIEU VENTOY (CHUNG DIA VOI VTOYEFI HOAC NHAN VENTOY)
// ============================================================================
static std::wstring FindVentoyDataPartition() {
    DWORD drives = GetLogicalDrives();
    DWORD ventoyDisk = (DWORD)-1;
    std::wstring fallbackDrive;

    for (int i = 0; i < 26; i++) {
        if (!(drives & (1 << i))) continue;
        wchar_t letter = (wchar_t)(L'A' + i);
        wchar_t root[4] = { letter, L':', L'\\', 0 };
        wchar_t volName[MAX_PATH] = { 0 };

        if (GetVolumeInformationW(root, volName, MAX_PATH, NULL, NULL, NULL, NULL, 0)) {
            std::wstring label = volName;
            std::transform(label.begin(), label.end(), label.begin(), [](wchar_t c) { return (wchar_t)std::towupper(c); });

            if (label == L"VTOYEFI") {
                DWORD dNum = 0;
                uint64_t pOff = 0;
                if (GetVolumeDiskInfo(letter, dNum, pOff)) {
                    ventoyDisk = dNum;
                }
            } else if (label.find(L"VENTOY") != std::wstring::npos && fallbackDrive.empty()) {
                fallbackDrive = root;
            }
        }
    }

    if (ventoyDisk != (DWORD)-1) {
        for (int i = 0; i < 26; i++) {
            if (!(drives & (1 << i))) continue;
            wchar_t letter = (wchar_t)(L'A' + i);
            wchar_t root[4] = { letter, L':', L'\\', 0 };
            wchar_t volName[MAX_PATH] = { 0 };

            if (GetVolumeInformationW(root, volName, MAX_PATH, NULL, NULL, NULL, NULL, 0)) {
                std::wstring label = volName;
                std::transform(label.begin(), label.end(), label.begin(), [](wchar_t c) { return (wchar_t)std::towupper(c); });
                if (label == L"VTOYEFI") continue;

                DWORD dNum = 0;
                uint64_t pOff = 0;
                if (GetVolumeDiskInfo(letter, dNum, pOff) && dNum == ventoyDisk) {
                    return root;
                }
            }
        }
    }

    return fallbackDrive;
}

// ============================================================================
// TAO FILE .VLNK 32 KB TRUC TIEP
// ============================================================================
static bool IsSupportedExt(const std::wstring& extLower) {
    static const wchar_t* exts[] = { L".iso", L".wim", L".vhd", L".vhdx", L".img", L".efi", L".vtoy", L".dat" };
    for (auto e : exts) {
        if (extLower == e) return true;
    }
    return false;
}

static int CreateNativeVlink(const std::wstring& srcPath, const std::wstring& outDir, std::wstring& errMsg) {
    if (srcPath.size() < 5 || srcPath[1] != L':') {
        errMsg = L"Đường dẫn nguồn không hợp lệ.";
        return 0;
    }

    size_t dotPos = srcPath.find_last_of(L'.');
    size_t slashPos = srcPath.find_last_of(L"\\/");
    if (dotPos == std::wstring::npos || (slashPos != std::wstring::npos && dotPos < slashPos)) {
        return -1;
    }

    std::wstring extLower = srcPath.substr(dotPos);
    std::transform(extLower.begin(), extLower.end(), extLower.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    if (!IsSupportedExt(extLower)) return -1;

    std::wstring baseName = (slashPos != std::wstring::npos)
        ? srcPath.substr(slashPos + 1, dotPos - slashPos - 1)
        : srcPath.substr(0, dotPos);

    // 1. Lay DiskNumber, PartitionOffset & DiskSignature
    DWORD diskNum = 0;
    uint64_t partOffset = 0;
    uint32_t diskSig = 0;
    if (!GetVolumeDiskInfo(srcPath[0], diskNum, partOffset)) {
        errMsg = L"Không đọc được Volume Disk Extents (cần chạy quyền Admin).";
        return 0;
    }
    if (!GetPhysicalDiskSignature(diskNum, diskSig)) {
        errMsg = L"Không đọc được Disk Signature của PhysicalDrive.";
        return 0;
    }

    // 2. Chuyen duong dan tuong doi sang UTF-8 dang "/ThuMuc/File.iso"
    std::wstring relPathW = srcPath.substr(2);
    std::replace(relPathW.begin(), relPathW.end(), L'\\', L'/');
    if (relPathW.empty() || relPathW[0] != L'/') relPathW = L"/" + relPathW;

    char utf8Path[VLNK_NAME_MAX] = { 0 };
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, relPathW.c_str(), -1, utf8Path, VLNK_NAME_MAX, NULL, NULL);
    if (utf8Len <= 0 || utf8Len >= VLNK_NAME_MAX) {
        errMsg = L"Đường dẫn quá dài (>= 384 bytes UTF-8).";
        return 0;
    }

    // 3. Dung buffer 32 KB & header ventoy_vlnk
    std::vector<uint8_t> buf(VLNK_FILE_LEN, 0);
    ventoy_vlnk* vlnk = (ventoy_vlnk*)buf.data();

    static const uint8_t ventoyGuid[16] = {
        0x20, 0x20, 0x77, 0x77, 0x77, 0x2E, 0x76, 0x65,
        0x6E, 0x74, 0x6F, 0x79, 0x2E, 0x6E, 0x65, 0x74
    };
    memcpy(vlnk->guid, ventoyGuid, 16);
    vlnk->crc32 = 0;
    vlnk->disk_signature = diskSig;
    vlnk->part_offset = partOffset;
    memcpy(vlnk->filepath, utf8Path, utf8Len);

    vlnk->crc32 = CalcCRC32C(vlnk, sizeof(ventoy_vlnk));

    // 4. Ghi file .vlnk 32 KB
    std::wstring destFolder = outDir;
    if (!destFolder.empty() && destFolder.back() != L'\\') destFolder += L'\\';
    std::wstring outFile = destFolder + baseName + L".vlnk" + extLower;

    HANDLE hFile = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        errMsg = L"Không thể tạo file đích: " + outFile;
        return 0;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, buf.data(), VLNK_FILE_LEN, &written, NULL);
    CloseHandle(hFile);

    if (!ok || written != VLNK_FILE_LEN) {
        errMsg = L"Ghi file không đủ 32 KB.";
        return 0;
    }
    return 1;
}

// ============================================================================
// XU LY CHON NHIEU FILE & TAO HANG LOAT
// ============================================================================
static void RunBatchVlink(HWND hWnd) {
    wchar_t destBuf[MAX_PATH] = { 0 };
    GetWindowTextW(g_hInpDest, destBuf, MAX_PATH);
    std::wstring destDir = destBuf;

    if (destDir.empty() || GetFileAttributesW(destDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hWnd, L"Vui lòng chọn phân vùng hoặc thư mục đích hợp lệ trên Ventoy.", L"Thiếu thư mục đích", MB_ICONERROR);
        return;
    }

    std::vector<wchar_t> fileBuf(65536, 0);
    OPENFILENAMEW ofn = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFilter = L"Boot Images (*.iso;*.wim;*.vhd;*.vhdx;*.img;*.efi)\0*.iso;*.wim;*.vhd;*.vhdx;*.img;*.efi\0"
                      L"ISO Images (*.iso)\0*.iso\0"
                      L"WIM Images (*.wim)\0*.wim\0"
                      L"VHD/VHDX (*.vhd;*.vhdx)\0*.vhd;*.vhdx\0"
                      L"IMG/EFI (*.img;*.efi)\0*.img;*.efi\0";
    ofn.lpstrFile = fileBuf.data();
    ofn.nMaxFile = (DWORD)fileBuf.size();
    ofn.lpstrTitle = L"Chọn nhiều file ISO, WIM, VHD, IMG, EFI để tạo Vlink";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return;

    std::vector<std::wstring> files;
    const wchar_t* p = fileBuf.data();
    std::wstring first = p;
    p += first.size() + 1;

    if (*p == 0) {
        files.push_back(first);
    } else {
        if (!first.empty() && first.back() != L'\\') first += L'\\';
        while (*p != 0) {
            std::wstring name = p;
            files.push_back(first + name);
            p += name.size() + 1;
        }
    }

    int success = 0, failed = 0, skipped = 0;
    std::wstring lastErr;

    for (size_t i = 0; i < files.size(); i++) {
        std::wstring status = L"Đang tạo vlink (" + std::to_wstring(i + 1) + L"/" + std::to_wstring(files.size()) + L")...";
        SetWindowTextW(g_hLblStatus, status.c_str());

        int res = CreateNativeVlink(files[i], destDir, lastErr);
        if (res == 1) success++;
        else if (res == -1) skipped++;
        else failed++;
    }

    std::wstring summary = L"Hoàn tất! Đích: " + destDir + L"\r\n" +
                           L"Thành công: " + std::to_wstring(success) +
                           L" | Bỏ qua: " + std::to_wstring(skipped) +
                           L" | Thất bại: " + std::to_wstring(failed);
    if (failed > 0 && !lastErr.empty()) summary += L"\r\nLỗi gần nhất: " + lastErr;

    SetWindowTextW(g_hLblStatus, summary.c_str());
    MessageBoxW(hWnd, summary.c_str(), L"Kết quả tạo Vlink", MB_ICONINFORMATION);
}

static void BrowseDestFolder(HWND hWnd) {
    BROWSEINFOW bi = { 0 };
    bi.hwndOwner = hWnd;
    bi.lpszTitle = L"Chọn phân vùng / thư mục đích trên Ventoy:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = { 0 };
        if (SHGetPathFromIDListW(pidl, path)) {
            std::wstring sPath = path;
            if (!sPath.empty() && sPath.back() != L'\\') sPath += L'\\';
            SetWindowTextW(g_hInpDest, sPath.c_str());
        }
        CoTaskMemFree(pidl);
    }
}

// ============================================================================
// CUA SO WIN32 GUI
// ============================================================================
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            HFONT hFontBold = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            HWND hLbl1 = CreateWindowW(L"STATIC", L"1. Phân vùng / Thư mục Ventoy:",
                                       WS_VISIBLE | WS_CHILD, 15, 22, 195, 22, hWnd, NULL, NULL, NULL);
            std::wstring detected = FindVentoyDataPartition();
            g_hInpDest = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", detected.c_str(),
                                         WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL, 215, 19, 245, 26, hWnd, (HMENU)IDC_INP_DEST, NULL, NULL);
            HWND hBtnDest = CreateWindowW(L"BUTTON", L"Chọn...",
                                          WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 470, 18, 75, 28, hWnd, (HMENU)IDC_BTN_DEST, NULL, NULL);

            HWND hBtnRun = CreateWindowW(L"BUTTON", L"2. Chọn nhiều file (ISO, WIM, VHD, IMG, EFI) && Tạo Vlink",
                                         WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 15, 62, 530, 42, hWnd, (HMENU)IDC_BTN_RUN, NULL, NULL);

            g_hLblStatus = CreateWindowW(L"STATIC", L"Sẵn sàng. Bản C++ Native ghi trực tiếp file .vlnk 32 KB chuẩn Ventoy.",
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

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitialize(NULL);
    InitCRC32CTable();

    const wchar_t CLASS_NAME[] = L"MultiVentoyVlnkCppClass";
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hWnd = CreateWindowExW(0, CLASS_NAME, L"Multi Ventoy Vlink Creator (C++ Native)",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 575, 245, NULL, NULL, hInstance, NULL);
    if (!hWnd) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
