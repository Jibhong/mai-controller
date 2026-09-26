#include <windows.h>
#include <process.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gui.h"
#include "mai2io.h"
#include "touch_zones.h"

// Constants for rendering
#define WINDOW_WIDTH 800
#define WINDOW_HEIGHT 850
#define PANEL_SIZE 800
#define CENTER_X 400
#define CENTER_Y 400
#define RADIUS_OUTER 350
#define RADIUS_INNER (int)(RADIUS_OUTER * 0.28)
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Thread sync
static CRITICAL_SECTION gui_lock;
static bool gui_running = false;
static HANDLE gui_thread = NULL;
static HWND gui_hwnd = NULL;

// Shared state for multi-process
struct mai2_shared_state {
    uint8_t touch_state_1p[7];
    uint8_t touch_state_2p[7];
    uint16_t gamebtn_state_1p;
    uint16_t gamebtn_state_2p;
    uint8_t opbtn_state;
};

static HANDLE hMapFile = NULL;
static struct mai2_shared_state *shared_state = NULL;
static HANDLE hMutex = NULL;

static int current_player_tab = 1;

// Decompressed lookup table (800*800 = 640000 bytes)
static uint8_t *zone_lut = NULL;

// UI Rectangles
static RECT tab1_rect = { 10, 10, 100, 40 };
static RECT tab2_rect = { 110, 10, 200, 40 };
static RECT test_btn_rect = { 10, 780, 100, 810 };
static RECT service_btn_rect = { 110, 780, 200, 810 };
static RECT coin_btn_rect = { 210, 780, 300, 810 };

// Sensor names for drawing labels
static const char *sensor_names[42] = {
    "A1","A2","A3","A4","A5","A6","A7","A8",
    "B1","B2","B3","B4","B5","B6","B7","B8",
    "C1","C2",
    "D1","D2","D3","D4","D5","D6","D7","D8",
    "E1","E2","E3","E4","E5","E6","E7","E8",
    "1","2","3","4","5","6","7","8"
};

// Precomputed label positions (center of each zone)
static POINT zone_label_pos[42];
static bool zone_labels_computed = false;

static void decompress_zone_lut(void) {
    if (zone_lut != NULL) return;
    zone_lut = (uint8_t *)malloc(TOUCH_ZONE_WIDTH * TOUCH_ZONE_HEIGHT);
    if (!zone_lut) return;
    
    int out = 0;
    for (int i = 0; i < TOUCH_ZONE_RLE_LEN; i++) {
        int count = (touch_zone_rle[i][0] << 8) | touch_zone_rle[i][1];
        uint8_t val = touch_zone_rle[i][2];
        for (int j = 0; j < count && out < TOUCH_ZONE_WIDTH * TOUCH_ZONE_HEIGHT; j++) {
            zone_lut[out++] = val;
        }
    }
}

static void compute_zone_labels(void) {
    if (zone_labels_computed || !zone_lut) return;
    
    // Accumulate pixel positions per zone to find centroids
    long long sum_x[42] = {0};
    long long sum_y[42] = {0};
    long long count[42] = {0};
    
    for (int py = 0; py < TOUCH_ZONE_HEIGHT; py++) {
        for (int px = 0; px < TOUCH_ZONE_WIDTH; px++) {
            uint8_t z = zone_lut[py * TOUCH_ZONE_WIDTH + px];
            if (z != TOUCH_ZONE_NONE && z < 42) {
                sum_x[z] += px;
                sum_y[z] += py;
                count[z]++;
            }
        }
    }
    
    for (int i = 0; i < 42; i++) {
        if (count[i] > 0) {
            zone_label_pos[i].x = (LONG)(sum_x[i] / count[i]);
            zone_label_pos[i].y = (LONG)(sum_y[i] / count[i]);
        }
    }
    zone_labels_computed = true;
}

static uint8_t lookup_zone(int x, int y) {
    if (!zone_lut) return TOUCH_ZONE_NONE;
    if (x < 0 || x >= TOUCH_ZONE_WIDTH || y < 0 || y >= TOUCH_ZONE_HEIGHT) return TOUCH_ZONE_NONE;
    return zone_lut[y * TOUCH_ZONE_WIDTH + x];
}

// Convert touch zone to bit inside the 7-byte array
static void set_touch_bit(uint8_t *state, int sensor_index, bool pressed) {
    int byteIndex = sensor_index / 5;
    int bitIndex = sensor_index % 5;
    if (pressed) {
        state[byteIndex] |= (1 << bitIndex);
    } else {
        state[byteIndex] &= ~(1 << bitIndex);
    }
}

static bool is_touch_bit_set(const uint8_t *state, int sensor_index) {
    int byteIndex = sensor_index / 5;
    int bitIndex = sensor_index % 5;
    return (state[byteIndex] & (1 << bitIndex)) != 0;
}

// Process mouse clicks
static void handle_mouse_down(HWND hwnd, int x, int y) {
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int cw = clientRect.right - clientRect.left;
    int ch = clientRect.bottom - clientRect.top;
    if (cw > 0 && ch > 0) {
        x = x * WINDOW_WIDTH / cw;
        y = y * WINDOW_HEIGHT / ch;
    }

    if (!shared_state) return;
    EnterCriticalSection(&gui_lock);

    // Check tabs
    if (PtInRect(&tab1_rect, (POINT){x, y})) current_player_tab = 1;
    if (PtInRect(&tab2_rect, (POINT){x, y})) current_player_tab = 2;

    // Check operator buttons
    if (PtInRect(&test_btn_rect, (POINT){x, y})) shared_state->opbtn_state |= MAI2_IO_OPBTN_TEST;
    if (PtInRect(&service_btn_rect, (POINT){x, y})) shared_state->opbtn_state |= MAI2_IO_OPBTN_SERVICE;
    if (PtInRect(&coin_btn_rect, (POINT){x, y})) shared_state->opbtn_state |= MAI2_IO_OPBTN_COIN;

    // Lookup from the generated zone table
    uint8_t zone = lookup_zone(x, y);

    uint16_t *gamebtn_state = (current_player_tab == 1) ? &shared_state->gamebtn_state_1p : &shared_state->gamebtn_state_2p;
    uint8_t *touch_state = (current_player_tab == 1) ? shared_state->touch_state_1p : shared_state->touch_state_2p;

    if (zone != TOUCH_ZONE_NONE) {
        if (zone >= TOUCH_ZONE_BTN_BASE && zone < TOUCH_ZONE_BTN_BASE + 8) {
            // Game button
            int btn = zone - TOUCH_ZONE_BTN_BASE;
            *gamebtn_state |= (1 << btn);
        } else if (zone == 16) {
            // C1 + Select
            set_touch_bit(touch_state, 16, true);
            *gamebtn_state |= MAI2_IO_GAMEBTN[8];
        } else if (zone == 17) {
            // C2 + Select
            set_touch_bit(touch_state, 17, true);
            *gamebtn_state |= MAI2_IO_GAMEBTN[8];
        } else if (zone < 34) {
            // Touch sensor
            set_touch_bit(touch_state, zone, true);
        }
    }

    LeaveCriticalSection(&gui_lock);
    InvalidateRect(gui_hwnd, NULL, FALSE);
}

static void handle_mouse_up(HWND hwnd, int x, int y) {
    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    int cw = clientRect.right - clientRect.left;
    int ch = clientRect.bottom - clientRect.top;
    if (cw > 0 && ch > 0) {
        x = x * WINDOW_WIDTH / cw;
        y = y * WINDOW_HEIGHT / ch;
    }

    if (!shared_state) return;
    EnterCriticalSection(&gui_lock);

    // Operator buttons
    if (PtInRect(&test_btn_rect, (POINT){x, y})) shared_state->opbtn_state &= ~MAI2_IO_OPBTN_TEST;
    if (PtInRect(&service_btn_rect, (POINT){x, y})) shared_state->opbtn_state &= ~MAI2_IO_OPBTN_SERVICE;
    if (PtInRect(&coin_btn_rect, (POINT){x, y})) shared_state->opbtn_state &= ~MAI2_IO_OPBTN_COIN;

    uint16_t *gamebtn_state = (current_player_tab == 1) ? &shared_state->gamebtn_state_1p : &shared_state->gamebtn_state_2p;
    uint8_t *touch_state = (current_player_tab == 1) ? shared_state->touch_state_1p : shared_state->touch_state_2p;

    // Clear everything for the active player on mouse up.
    *gamebtn_state = 0;
    memset(touch_state, 0, 7);

    LeaveCriticalSection(&gui_lock);
    InvalidateRect(gui_hwnd, NULL, FALSE);
}

// Color table for zones (COLORREF)
static COLORREF zone_colors[42] = {
    // A1-A8: blue-ish
    RGB(60,80,160), RGB(60,80,160), RGB(60,80,160), RGB(60,80,160),
    RGB(60,80,160), RGB(60,80,160), RGB(60,80,160), RGB(60,80,160),
    // B1-B8: teal
    RGB(40,140,120), RGB(40,140,120), RGB(40,140,120), RGB(40,140,120),
    RGB(40,140,120), RGB(40,140,120), RGB(40,140,120), RGB(40,140,120),
    // C1, C2: purple
    RGB(100,50,130), RGB(100,50,130),
    // D1-D8: dark blue
    RGB(50,60,120), RGB(50,60,120), RGB(50,60,120), RGB(50,60,120),
    RGB(50,60,120), RGB(50,60,120), RGB(50,60,120), RGB(50,60,120),
    // E1-E8: dark teal
    RGB(30,100,90), RGB(30,100,90), RGB(30,100,90), RGB(30,100,90),
    RGB(30,100,90), RGB(30,100,90), RGB(30,100,90), RGB(30,100,90),
    // Buttons 1-8: dark gray
    RGB(70,70,70), RGB(70,70,70), RGB(70,70,70), RGB(70,70,70),
    RGB(70,70,70), RGB(70,70,70), RGB(70,70,70), RGB(70,70,70),
};

static COLORREF zone_colors_active[42] = {
    // A1-A8 active: bright blue
    RGB(100,140,255), RGB(100,140,255), RGB(100,140,255), RGB(100,140,255),
    RGB(100,140,255), RGB(100,140,255), RGB(100,140,255), RGB(100,140,255),
    // B1-B8 active: bright teal
    RGB(80,220,200), RGB(80,220,200), RGB(80,220,200), RGB(80,220,200),
    RGB(80,220,200), RGB(80,220,200), RGB(80,220,200), RGB(80,220,200),
    // C1, C2 active: bright purple
    RGB(180,100,240), RGB(180,100,240),
    // D1-D8 active: medium blue
    RGB(90,110,220), RGB(90,110,220), RGB(90,110,220), RGB(90,110,220),
    RGB(90,110,220), RGB(90,110,220), RGB(90,110,220), RGB(90,110,220),
    // E1-E8 active: bright teal-green
    RGB(60,200,170), RGB(60,200,170), RGB(60,200,170), RGB(60,200,170),
    RGB(60,200,170), RGB(60,200,170), RGB(60,200,170), RGB(60,200,170),
    // Buttons 1-8 active: bright red
    RGB(220,60,60), RGB(220,60,60), RGB(220,60,60), RGB(220,60,60),
    RGB(220,60,60), RGB(220,60,60), RGB(220,60,60), RGB(220,60,60),
};

static bool is_zone_active(uint8_t zone, const uint8_t *touch_state, uint16_t gamebtn_state) {
    if (zone == TOUCH_ZONE_NONE) return false;
    if (zone >= TOUCH_ZONE_BTN_BASE && zone < TOUCH_ZONE_BTN_BASE + 8) {
        return (gamebtn_state & (1 << (zone - TOUCH_ZONE_BTN_BASE))) != 0;
    }
    if (zone < 34) {
        return is_touch_bit_set(touch_state, zone);
    }
    return false;
}

static void scale_rect(RECT* dest, const RECT* src, float scale_x, float scale_y) {
    dest->left = (int)(src->left * scale_x);
    dest->right = (int)(src->right * scale_x);
    dest->top = (int)(src->top * scale_y);
    dest->bottom = (int)(src->bottom * scale_y);
}

// Window Procedure
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZING: {
            RECT* prc = (RECT*)lParam;
            int fwSide = (int)wParam;
            
            RECT wr = {0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
            AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST);
            int nc_width = (wr.right - wr.left) - WINDOW_WIDTH;
            int nc_height = (wr.bottom - wr.top) - WINDOW_HEIGHT;

            int w = prc->right - prc->left;
            int h = prc->bottom - prc->top;
            int cw = w - nc_width;
            int ch = h - nc_height;

            if (fwSide == WMSZ_LEFT || fwSide == WMSZ_RIGHT) {
                ch = cw * WINDOW_HEIGHT / WINDOW_WIDTH;
            } else if (fwSide == WMSZ_TOP || fwSide == WMSZ_BOTTOM) {
                cw = ch * WINDOW_WIDTH / WINDOW_HEIGHT;
            } else {
                ch = cw * WINDOW_HEIGHT / WINDOW_WIDTH;
            }

            w = cw + nc_width;
            h = ch + nc_height;

            if (fwSide == WMSZ_LEFT || fwSide == WMSZ_TOPLEFT || fwSide == WMSZ_BOTTOMLEFT) prc->left = prc->right - w;
            else prc->right = prc->left + w;

            if (fwSide == WMSZ_TOP || fwSide == WMSZ_TOPLEFT || fwSide == WMSZ_TOPRIGHT) prc->top = prc->bottom - h;
            else prc->bottom = prc->top + h;

            return TRUE;
        }
        case WM_LBUTTONDOWN:
            handle_mouse_down(hwnd, LOWORD(lParam), HIWORD(lParam));
            SetCapture(hwnd);
            break;
        case WM_LBUTTONUP:
            handle_mouse_up(hwnd, LOWORD(lParam), HIWORD(lParam));
            ReleaseCapture();
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT clientRect;
            GetClientRect(hwnd, &clientRect);
            int cw = clientRect.right - clientRect.left;
            int ch = clientRect.bottom - clientRect.top;

            if (cw > 0 && ch > 0) {
                float scale_x = (float)cw / WINDOW_WIDTH;
                float scale_y = (float)ch / WINDOW_HEIGHT;

                // Double buffering at native scaled resolution
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP memBitmap = CreateCompatibleBitmap(hdc, cw, ch);
                HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);

                // Fill background
                HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 30));
                RECT fullRect = {0, 0, cw, ch};
                FillRect(memDC, &fullRect, bgBrush);
                DeleteObject(bgBrush);

                SetBkMode(memDC, TRANSPARENT);

                if (shared_state) {
                    EnterCriticalSection(&gui_lock);

                    const uint8_t *current_touch = (current_player_tab == 1) ? shared_state->touch_state_1p : shared_state->touch_state_2p;
                    uint16_t current_gamebtn = (current_player_tab == 1) ? shared_state->gamebtn_state_1p : shared_state->gamebtn_state_2p;

                    int scaled_panel_size = (int)(PANEL_SIZE * scale_x);
                    if (scaled_panel_size < 1) scaled_panel_size = 1;

                    BITMAPINFO bmi = {0};
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = scaled_panel_size;
                    bmi.bmiHeader.biHeight = -scaled_panel_size; // top-down
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;

                    uint32_t *pixels = NULL;
                    HBITMAP panelBmp = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
                    
                    if (panelBmp && pixels && zone_lut) {
                        HDC panelDC = CreateCompatibleDC(memDC);
                        SelectObject(panelDC, panelBmp);

                        for (int py = 0; py < scaled_panel_size; py++) {
                            for (int px = 0; px < scaled_panel_size; px++) {
                                int orig_x = px * PANEL_SIZE / scaled_panel_size;
                                int orig_y = py * PANEL_SIZE / scaled_panel_size;
                                uint8_t z = zone_lut[orig_y * TOUCH_ZONE_WIDTH + orig_x];
                                COLORREF c;
                                if (z == TOUCH_ZONE_NONE) {
                                    c = RGB(30, 30, 30);
                                } else if (z < 42) {
                                    bool active = is_zone_active(z, current_touch, current_gamebtn);
                                    c = active ? zone_colors_active[z] : zone_colors[z];
                                } else {
                                    c = RGB(30, 30, 30);
                                }
                                pixels[py * scaled_panel_size + px] = (GetBValue(c) << 16) | (GetGValue(c) << 8) | GetRValue(c);
                            }
                        }

                        for (int py = 0; py < scaled_panel_size - 1; py++) {
                            for (int px = 0; px < scaled_panel_size - 1; px++) {
                                int orig_x = px * PANEL_SIZE / scaled_panel_size;
                                int orig_y = py * PANEL_SIZE / scaled_panel_size;
                                uint8_t z = zone_lut[orig_y * TOUCH_ZONE_WIDTH + orig_x];
                                
                                int orig_xr = (px + 1) * PANEL_SIZE / scaled_panel_size;
                                int orig_yd = (py + 1) * PANEL_SIZE / scaled_panel_size;
                                uint8_t zr = zone_lut[orig_y * TOUCH_ZONE_WIDTH + orig_xr];
                                uint8_t zd = zone_lut[orig_yd * TOUCH_ZONE_WIDTH + orig_x];
                                
                                if (z != zr || z != zd) {
                                    pixels[py * scaled_panel_size + px] = 0x00505050; // gray border
                                }
                            }
                        }

                        BitBlt(memDC, 0, 0, scaled_panel_size, scaled_panel_size, panelDC, 0, 0, SRCCOPY);
                        DeleteDC(panelDC);
                        DeleteObject(panelBmp);
                    }

                    if (zone_labels_computed) {
                        int font_size = (int)(14 * scale_y);
                        if (font_size < 8) font_size = 8;
                        HFONT font = CreateFont(font_size, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, "Consolas");
                        HFONT oldFont = (HFONT)SelectObject(memDC, font);
                        SetTextColor(memDC, RGB(255, 255, 255));

                        for (int i = 0; i < 42; i++) {
                            if (zone_label_pos[i].x == 0 && zone_label_pos[i].y == 0) continue;
                            RECT lr = {
                                (int)(zone_label_pos[i].x * scale_x) - (int)(20 * scale_x),
                                (int)(zone_label_pos[i].y * scale_y) - (int)(8 * scale_y),
                                (int)(zone_label_pos[i].x * scale_x) + (int)(20 * scale_x),
                                (int)(zone_label_pos[i].y * scale_y) + (int)(8 * scale_y)
                            };
                            DrawText(memDC, sensor_names[i], -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        }

                        SelectObject(memDC, oldFont);
                        DeleteObject(font);
                    }

                    HBRUSH activeTabBrush = CreateSolidBrush(RGB(100, 100, 200));
                    HBRUSH inactiveTabBrush = CreateSolidBrush(RGB(50, 50, 50));
                    SetTextColor(memDC, RGB(255, 255, 255));

                    int ui_font_size = (int)(16 * scale_y);
                    if (ui_font_size < 10) ui_font_size = 10;
                    HFONT uiFont = CreateFont(ui_font_size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, "Arial");
                    HFONT oldUIFont = (HFONT)SelectObject(memDC, uiFont);

                    RECT t1; scale_rect(&t1, &tab1_rect, scale_x, scale_y);
                    RECT t2; scale_rect(&t2, &tab2_rect, scale_x, scale_y);
                    FillRect(memDC, &t1, current_player_tab == 1 ? activeTabBrush : inactiveTabBrush);
                    DrawText(memDC, "Player 1", -1, &t1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    FillRect(memDC, &t2, current_player_tab == 2 ? activeTabBrush : inactiveTabBrush);
                    DrawText(memDC, "Player 2", -1, &t2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    RECT bt; scale_rect(&bt, &test_btn_rect, scale_x, scale_y);
                    RECT bs; scale_rect(&bs, &service_btn_rect, scale_x, scale_y);
                    RECT bc; scale_rect(&bc, &coin_btn_rect, scale_x, scale_y);
                    FillRect(memDC, &bt, (shared_state->opbtn_state & MAI2_IO_OPBTN_TEST) ? activeTabBrush : inactiveTabBrush);
                    DrawText(memDC, "Test", -1, &bt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    FillRect(memDC, &bs, (shared_state->opbtn_state & MAI2_IO_OPBTN_SERVICE) ? activeTabBrush : inactiveTabBrush);
                    DrawText(memDC, "Service", -1, &bs, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    FillRect(memDC, &bc, (shared_state->opbtn_state & MAI2_IO_OPBTN_COIN) ? activeTabBrush : inactiveTabBrush);
                    DrawText(memDC, "Coin", -1, &bc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                    SelectObject(memDC, oldUIFont);
                    DeleteObject(uiFont);
                    DeleteObject(activeTabBrush);
                    DeleteObject(inactiveTabBrush);

                    LeaveCriticalSection(&gui_lock);
                }

                // Native resolution blit! No stretching needed
                BitBlt(hdc, 0, 0, cw, ch, memDC, 0, 0, SRCCOPY);

                SelectObject(memDC, oldBitmap);
                DeleteObject(memBitmap);
                DeleteDC(memDC);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Thread Procedure
static unsigned int __stdcall gui_thread_proc(void *ctx) {
    WNDCLASS wc = {0};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "Mai2TouchGUI";
    RegisterClass(&wc);

    DWORD exStyle = WS_EX_APPWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    DWORD style = WS_OVERLAPPEDWINDOW;

    RECT wr = { 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT };
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;

    gui_hwnd = CreateWindowEx(
        exStyle, "Mai2TouchGUI", "Mai Input GUI",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        NULL, NULL, wc.hInstance, NULL
    );

    ShowWindow(gui_hwnd, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    gui_running = false;
    return 0;
}

void mai2_gui_init(void) {
    if (shared_state != NULL) return;

    // Decompress zone lookup table
    decompress_zone_lut();
    compute_zone_labels();

    // Set up shared memory
    hMapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(struct mai2_shared_state),
        "Local\\Mai2TouchGUI_SharedMemory");

    if (hMapFile == NULL) return;

    shared_state = (struct mai2_shared_state *) MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct mai2_shared_state));
    if (shared_state == NULL) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return;
    }

    InitializeCriticalSection(&gui_lock);

    // Set up mutex to elect who spawns the window
    hMutex = CreateMutex(NULL, TRUE, "Local\\Mai2TouchGUI_WindowMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        hMutex = NULL;
        gui_running = true;
    } else {
        memset(shared_state, 0, sizeof(struct mai2_shared_state));
        gui_running = true;
        gui_thread = (HANDLE)_beginthreadex(NULL, 0, gui_thread_proc, NULL, 0, NULL);
    }
}

void mai2_gui_shutdown(void) {
    if (!gui_running) return;

    if (hMutex != NULL) {
        if (gui_hwnd) {
            PostMessage(gui_hwnd, WM_CLOSE, 0, 0);
        }
        WaitForSingleObject(gui_thread, INFINITE);
        CloseHandle(gui_thread);
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    DeleteCriticalSection(&gui_lock);

    if (shared_state) {
        UnmapViewOfFile(shared_state);
        shared_state = NULL;
    }
    if (hMapFile) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
    }
    if (zone_lut) {
        free(zone_lut);
        zone_lut = NULL;
    }

    gui_running = false;
}

void mai2_gui_get_touch_state(int player, uint8_t state[7]) {
    if (!gui_running || !shared_state) return;
    EnterCriticalSection(&gui_lock);
    memcpy(state, player == 1 ? shared_state->touch_state_1p : shared_state->touch_state_2p, 7);
    LeaveCriticalSection(&gui_lock);
}

void mai2_gui_get_gamebtns(int player, uint16_t *btns) {
    if (!gui_running || !shared_state) return;
    EnterCriticalSection(&gui_lock);
    *btns |= (player == 1) ? shared_state->gamebtn_state_1p : shared_state->gamebtn_state_2p;
    LeaveCriticalSection(&gui_lock);
}

void mai2_gui_get_opbtns(uint8_t *opbtn) {
    if (!gui_running || !shared_state) return;
    EnterCriticalSection(&gui_lock);
    *opbtn |= shared_state->opbtn_state;
    LeaveCriticalSection(&gui_lock);
}
