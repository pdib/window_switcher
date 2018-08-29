#include <Windows.h>
#include <Windowsx.h>
#include <dwmapi.h>
#include <memory>
#include <algorithm>
#include <Psapi.h>
#include <bitset>
#include <iostream>
#include <thread>
#include <iterator>
#include <string>
#include <vector>
#include <cctype>
#include <Shlwapi.h>
#include <shellapi.h>

constexpr auto c_W_KEY = 0x5A;
constexpr auto c_OVERLAY_WNDCLASS_NAME = "window_switcher_overlay_wndclass";
constexpr auto c_MIRROR_WNDCLASS_NAME = "window_switcher_mirror_wndclass";
constexpr unsigned int c_NOTIFY_ICON_MESSAGE = WM_APP + 0x0001;
constexpr unsigned int c_CLOSE_OVERLAY_WINDOW_MESSAGE = WM_APP + 0x0002;
constexpr unsigned int c_MENU_ITEM_QUIT = 0x0001;

std::thread g_overlay_window_thread;
HMENU g_notify_icon_context_menu = nullptr;

// Overlay window is the parent window invoked when pressing the main keyboard shortcut.
HWND g_overlay_hwnd = nullptr;

// Edit window, child of overlay window.
HWND g_edit_hwnd = nullptr;

// List box window, child of overlay window.
HWND g_list_box_hwnd = nullptr;

// Mirror window, replicates the display of the currently selected item in List box. Child of overlay window.
HWND g_mirror_hwnd = nullptr;

struct get_visible_windows_data
{
    std::vector<HWND> hwnds;
};

struct window_process_info
{
    window_process_info(
        HWND hwnd,
        DWORD pid,
        std::string&& window_title,
        std::string&& process_name)
        : hwnd(hwnd),
        pid(pid),
        window_title(std::move(window_title)),
        process_name(std::move(process_name))
    {
        static_assert(std::is_move_constructible<std::string>(), "");
    }

    HWND hwnd = 0;
    DWORD pid = 0;
    std::string window_title;
    std::string process_name;
};

BOOL __stdcall EnumWindowsProc(HWND hwnd, LPARAM lparam)
{
    auto& hwnds = reinterpret_cast<get_visible_windows_data*>(lparam)->hwnds;
    auto style = GetWindowLongPtr(hwnd, GWL_STYLE);
    auto parent_hwnd = GetWindowLongPtr(hwnd, GWLP_HWNDPARENT);

    if (!parent_hwnd &&
        IsWindow(hwnd) &&
        IsWindowVisible(hwnd) &&
        IsWindowEnabled(hwnd) &&
        (style & WS_OVERLAPPEDWINDOW) &&
        !(style & WS_POPUP))
    {
        hwnds.push_back(hwnd);
    }
    return true;
}

std::vector<HWND> GetVisibleWindows()
{
    get_visible_windows_data data;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return data.hwnds;
}

// Reads the process id, name and the window title of several HWNDs.
std::vector<window_process_info> PopulateWindowInformation(std::vector<HWND> const& hwnds)
{
    std::vector<window_process_info> wpis;
    for (auto hwnd : hwnds)
    {
        char process_name[100] = { 0 };
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        auto processHandle = OpenProcess(PROCESS_QUERY_INFORMATION, false, pid);
        if (!GetProcessImageFileName(processHandle, process_name, static_cast<DWORD>(std::size(process_name))))
        {
            // TODO(padib): handle errors
        }
        PTSTR filename = PathFindFileName(process_name);
        CloseHandle(processHandle);

        char title[100] = { 0 };
        if (!GetWindowText(hwnd, title, static_cast<int>(std::size(title))))
        {
            // TODO(padib): handle errors
        }
        wpis.emplace_back(hwnd, pid, std::string(title), std::string(filename));
    }

    return wpis;
}

// Match wpis against a single word query.
std::vector<size_t> QueryWindows(std::string & query, std::vector<window_process_info> const & wpis)
{
    std::vector<size_t> matching_indices;
    for (size_t i = 0; i < wpis.size(); ++i)
    {
        auto const& wpi = wpis[i];

        // Make all the stuff lower case
        auto window_title = wpi.window_title;
        auto process_name = wpi.process_name;
        std::transform(begin(window_title), end(window_title), begin(window_title), [](int c) { return static_cast<char>(std::tolower(c)); });
        std::transform(begin(process_name), end(process_name), begin(process_name), [](int c) { return static_cast<char>(std::tolower(c)); });
        std::transform(begin(query), end(query), begin(query), [](int c) { return static_cast<char>(std::tolower(c)); });

        // find word in window title or process name
        if (window_title.find(query) != std::string::npos || process_name.find(query) != std::string::npos)
        {
            matching_indices.push_back(i);
        }
    }
    return matching_indices;
}

// Fill an array of indices that tells what elements of wpis match the user query.
// Precond: 
// - wholeQuery is a null-terminated string of space-separated words.
// - matching_indices is empty
// Postcond:
// If idx is contained in matching_indices, it means that wpis[idx] matches the input wholeQuery.
void QueryWindows(char* wholeQuery, std::vector<window_process_info> const& wpis, std::vector<size_t>& matching_indices)
{
    char* next_token = nullptr;
    // Split query into words.
    // Do a matching pass for each word.
    auto token_ptr = strtok_s(wholeQuery, " ", &next_token);
    if (token_ptr)
    {
        std::string token(token_ptr);
        while (!token.empty())
        {
            auto indices = QueryWindows(token, wpis);
            for (auto idx : indices)
            {
                if (std::find(begin(matching_indices), end(matching_indices), idx) == end(matching_indices))
                {
                    matching_indices.push_back(idx);
                }
            }
            auto next = strtok_s(nullptr, " ", &next_token);
            if (next)
            {
                token = std::string(next);
            }
            else
            {
                token.clear();
            }
        }
    }
}

void RemoveNotifyIcon(NOTIFYICONDATA* p)
{
    Shell_NotifyIcon(NIM_DELETE, p);
}

using NotifyIconPtr = std::unique_ptr<NOTIFYICONDATA, decltype(&RemoveNotifyIcon)>;

NotifyIconPtr CreateNotifyIcon(HWND message_window)
{
    auto icon_data_ptr = NotifyIconPtr(new NOTIFYICONDATA, &RemoveNotifyIcon);
    auto error_result = NotifyIconPtr(nullptr, &RemoveNotifyIcon);

    // TODO(padib): Maybe we should be using NIF_GUID here
    // But the path to the binary file is encoded in the registration
    // Which makes Shell_NotifyIcon fails everytime we call it from a *different location* with the same GUID.
    NOTIFYICONDATA& icon_data = *icon_data_ptr;
    icon_data.cbSize = sizeof(NOTIFYICONDATA);
    icon_data.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP | NIF_MESSAGE;
    icon_data.uID = 0; // We only have single NotifyIcon, hardcoded id 0 should be enough.
    icon_data.uCallbackMessage = c_NOTIFY_ICON_MESSAGE;
    icon_data.hIcon = LoadIcon(nullptr, IDI_QUESTION);
    // This text will be shown as the icon's tooltip.
    strcpy_s(icon_data.szTip, "window_switcher.exe");
    icon_data.uVersion = NOTIFYICON_VERSION_4;
    icon_data.hWnd = message_window;

    // Now, let's bound the new notify to our window and add it.
    if (!Shell_NotifyIcon(NIM_ADD, &icon_data))
    {
        return error_result;
    }

    if (!Shell_NotifyIcon(NIM_SETVERSION, &icon_data))
    {
        return error_result;
    }

    return icon_data_ptr;
}

void CloseOverlayWindowFromOwnThread()
{
    DestroyWindow(g_overlay_hwnd);
    DestroyWindow(g_edit_hwnd);
    DestroyWindow(g_list_box_hwnd);
    DestroyWindow(g_mirror_hwnd);
    g_edit_hwnd = nullptr;
    g_list_box_hwnd = nullptr;
    g_overlay_hwnd = nullptr;
    g_mirror_hwnd = nullptr;
}

// DestroyWindow cannot destroy a window created by a different thread.
// To destroy the window, we first send a custom message to it.
// The window will destroy itself upon receiving the message.
void SendCloseOverlayWindowMessage()
{
    SendMessage(g_overlay_hwnd, c_CLOSE_OVERLAY_WINDOW_MESSAGE, 0 /*wParam*/, 0 /*lParam*/);
}

void RunMainLoop()
{
    bool isRunning = true;
    while (isRunning)
    {
        MSG message;
        if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                isRunning = false;
            }

            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    }
}

HWND GetCurrentlySelectedHwnd()
{
    int current_selection = ListBox_GetCurSel(g_list_box_hwnd);
    return (HWND)ListBox_GetItemData(g_list_box_hwnd, current_selection);
}

void SendCurrentlySelectedWindowToForeground()
{
    auto target_hwnd = GetCurrentlySelectedHwnd();
    if (target_hwnd)
    {
        if (IsIconic(target_hwnd))
        {
            SendMessage(target_hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);
        }
        SetForegroundWindow(target_hwnd);
    }
}

void RunOverlayWindowThreadLoop()
{
    bool isRunning = true;
    while (isRunning)
    {
        MSG message;

        // Deliberately listen to all messages destined to the thread. Not only a specific window's messages.
        // This allows us to get messages for the windows that are composing the overlay windows (e.g. edit_hwnd, list_box_hwnd).
        //
        // Messages that are destined to the overlay window are transmitted through this loop but they shouldn't be handled here.
        // They should be handled in the dedicated OverlayWindowProc.
        if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                isRunning = false;
            }

            // Listen to all keydown events, regardless of which window they're destined to.
            if (message.message == WM_KEYDOWN)
            {
                switch (message.wParam)
                {
                case VK_ESCAPE:
                {
                    CloseOverlayWindowFromOwnThread();
                } break;
                case VK_DOWN:
                {
                    int window_count = ListBox_GetCount(g_list_box_hwnd);
                    int current_selection = ListBox_GetCurSel(g_list_box_hwnd);
                    int next_item = min(current_selection + 1, window_count);
                    ListBox_SetCurSel(g_list_box_hwnd, next_item);
                } break;
                case VK_UP:
                {
                    int current_selection = ListBox_GetCurSel(g_list_box_hwnd);
                    int next_item = max(current_selection - 1, 0);
                    ListBox_SetCurSel(g_list_box_hwnd, next_item);
                } break;
                case VK_RETURN:
                {
                    SendCurrentlySelectedWindowToForeground();
                    CloseOverlayWindowFromOwnThread();
                } break;
                }
                RedrawWindow(g_mirror_hwnd, 0, 0, RDW_INVALIDATE | RDW_UPDATENOW);
            }

            // Clicking on an item in the ListBox is the same as hitting the Return key.
            if (message.message == WM_LBUTTONUP && message.hwnd == g_list_box_hwnd)
            {
                SendCurrentlySelectedWindowToForeground();
                CloseOverlayWindowFromOwnThread();
            }

            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    }
}

void AddItemToListBox(HWND list_box_hwnd, window_process_info const & wpi)
{
    auto list_item = ListBox_AddString(list_box_hwnd, (wpi.process_name + " - " + wpi.window_title).c_str());
    ListBox_SetItemData(list_box_hwnd, list_item, (LPVOID)wpi.hwnd);
}

void ClearAndDisplayWindowList(HWND list_box_hwnd, char * query)
{
    // Populate the list box
    auto hwnds = GetVisibleWindows();
    auto wpis = PopulateWindowInformation(hwnds);

    std::vector<size_t> matching_indices;
    QueryWindows(query, wpis, matching_indices);

    ListBox_ResetContent(list_box_hwnd);
    if (matching_indices.empty())
    {
        for (auto const & wpi : wpis)
        {
            if (wpi.hwnd != g_overlay_hwnd)
            {
                AddItemToListBox(list_box_hwnd, wpi);
            }
        }
    }
    else
    {
        for (int index = 0; index < matching_indices.size(); ++index)
        {
            auto const& wpi = wpis[matching_indices[index]];
            if (wpi.hwnd != g_overlay_hwnd)
            {
                AddItemToListBox(list_box_hwnd, wpi);
            }
        }
    }

    int initial_selection_index = 0;
    // We got an empty query. In that case, we start by selecting the second item
    // in the list. This enables a behavior similar to Alt-Tab (focusing the most
    // recently active window).
    if (query[0] == 0)
    {
        initial_selection_index = 1;
    }

    ListBox_SetCurSel(list_box_hwnd, initial_selection_index);
}

LRESULT MirrorWindowProc(
    _In_ HWND hWnd,
    _In_ UINT msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT:
    {
        PAINTSTRUCT paint_struct;
        BeginPaint(hWnd, &paint_struct);

        static auto s_brush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(paint_struct.hdc, &paint_struct.rcPaint, s_brush);

        auto source_hwnd = GetCurrentlySelectedHwnd();

        // Get destination rectangle by scaling the source rectangle to the 
        // current window client rect.
        RECT source_rect;
        GetWindowRect(source_hwnd, &source_rect);
        RECT available_rect;
        GetClientRect(hWnd, &available_rect);
        float width_ratio = static_cast<float>(available_rect.right - available_rect.left) / static_cast<float>(source_rect.right - source_rect.left);
        float height_ratio = static_cast<float>(available_rect.bottom - available_rect.top) / static_cast<float>(source_rect.bottom - source_rect.top);

        // Choose the lowest ratio (choosing the highest won't fit the other dimension in the dest window).
        float ratio = min(width_ratio, height_ratio);

        // If we don't have to reduce the source window size (i.e if dest_width > source_width && dest_height > source_height)
        // then keep the original window size.
        ratio = min(ratio, 1.f);

        // Fit the destination rectangle in the available rectangle so that it is centered.
        int needed_width = (source_rect.right - source_rect.left) * ratio;
        int needed_height = (source_rect.bottom - source_rect.top) * ratio;
        RECT dest_rect = {};
        dest_rect.left = ((available_rect.right - available_rect.left) - needed_width) / 2;
        dest_rect.right = dest_rect.left + needed_width;
        dest_rect.top = ((available_rect.bottom - available_rect.top) - needed_height) / 2;
        dest_rect.bottom = dest_rect.top + needed_height;

        static HTHUMBNAIL thumbnail_id = NULL;

        if (thumbnail_id)
        {
            DwmUnregisterThumbnail(thumbnail_id);
        }

        // Register and update the thumbnail using Desktop Window Manager APIs.
        // This allows us to display a thumbnail of the currently selected HWND in g_mirror_hwnd, 
        // just like the ALT+TAB window does.
        if (SUCCEEDED(DwmRegisterThumbnail(hWnd, source_hwnd, &thumbnail_id)))
        {
            // Set the thumbnail properties for use
            DWM_THUMBNAIL_PROPERTIES thumbnail_properties;
            thumbnail_properties.dwFlags = DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION;
            thumbnail_properties.fSourceClientAreaOnly = FALSE;
            thumbnail_properties.fVisible = TRUE;
            thumbnail_properties.rcDestination = dest_rect;

            // Display the thumbnail
            DwmUpdateThumbnailProperties(thumbnail_id, &thumbnail_properties);
        }

        EndPaint(hWnd, &paint_struct);
    } break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

LRESULT OverlayWindowProc(
    _In_ HWND hWnd,
    _In_ UINT msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
#ifdef DEBUG
    char log_buffer[200];
    sprintf_s(log_buffer, "Hwnd: %p - Message %lx - wParam %zx - lParam %zx\n", hWnd, (uint32_t)msg, (size_t)wParam, (size_t)lParam);
    OutputDebugString(log_buffer);
#endif

    if (msg == WM_COMMAND)
    {
        if (lParam != 0 && (HWND)lParam == g_edit_hwnd)
        {
            // Edit window notifications.
            switch (HIWORD(wParam))
            {
            case EN_CHANGE:
            {
                char input[100];
                ZeroMemory(input, std::size(input));
                Edit_GetText(g_edit_hwnd, input, static_cast<int>(std::size(input)));
                ClearAndDisplayWindowList(g_list_box_hwnd, input);
            } break;
            default:
            {
                return DefWindowProc(hWnd, msg, wParam, lParam);
            }
            }
        }
        else if (lParam != 0 && (HWND)lParam == g_list_box_hwnd)
        {
            // List box window notifications
            switch (HIWORD(wParam))
            {
            case LBN_SELCHANGE:
            {
                SendCurrentlySelectedWindowToForeground();
            } break;
            default:
            {
                return DefWindowProc(hWnd, msg, wParam, lParam);
            }
            }
        }
    }
    else if (msg == c_CLOSE_OVERLAY_WINDOW_MESSAGE)
    {
        CloseOverlayWindowFromOwnThread();
    }
    else if (msg == WM_ACTIVATEAPP && !wParam)
    {
        // This closes the overlay window whenever it loses focus.
        CloseOverlayWindowFromOwnThread();
    }
    else if (msg == WM_DESTROY)
    {
        PostQuitMessage(0);
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void CreateOverlayWindow()
{
    constexpr int desired_width = 900;
    constexpr int desired_height = 420;
    constexpr int edit_height = 20;
    constexpr int edit_width = 350;
    constexpr int mirror_width = desired_width - edit_width;
    constexpr int list_box_height = desired_height - edit_height;

    static_assert(list_box_height > 0, "Overlay window isn't tall enough to fit all components.");
    static_assert(mirror_width > 0, "Overlay window isn't wide enough to fit all components.");

    RECT desktop_rect;
    GetWindowRect(GetDesktopWindow(), &desktop_rect);

    int screen_center_x = desktop_rect.left / 2 + desktop_rect.right / 2;
    int screen_center_y = desktop_rect.top / 2 + desktop_rect.bottom / 2;

    int overlay_window_top_left_x = screen_center_x - desired_width / 2;
    int overlay_window_top_left_y = screen_center_y - desired_height / 2;

    g_overlay_hwnd = CreateWindowEx(
        WS_EX_TOOLWINDOW,
        c_OVERLAY_WNDCLASS_NAME,
        "",
        WS_VISIBLE,
        overlay_window_top_left_x,
        overlay_window_top_left_y,
        0,
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr);

    g_list_box_hwnd = CreateWindow(
        "ListBox",
        "",
        WS_BORDER | WS_POPUPWINDOW | WS_CHILD | WS_VISIBLE | LBS_NOINTEGRALHEIGHT,
        overlay_window_top_left_x,
        overlay_window_top_left_y + edit_height,
        edit_width,
        list_box_height,
        g_overlay_hwnd,
        nullptr,
        nullptr,
        nullptr);

    g_edit_hwnd = CreateWindow(
        "Edit",
        "",
        ES_LEFT | WS_BORDER | WS_POPUPWINDOW | WS_CHILD | WS_VISIBLE,
        overlay_window_top_left_x,
        overlay_window_top_left_y,
        edit_width,
        edit_height,
        g_overlay_hwnd,
        nullptr,
        nullptr,
        nullptr);

    g_mirror_hwnd = CreateWindow(
        c_MIRROR_WNDCLASS_NAME,
        "",
        WS_BORDER | WS_POPUPWINDOW | WS_CHILD | WS_VISIBLE,
        overlay_window_top_left_x + edit_width,
        overlay_window_top_left_y,
        mirror_width,
        desired_height,
        g_overlay_hwnd,
        nullptr,
        nullptr,
        nullptr);

    SetForegroundWindow(g_edit_hwnd);
    SetFocus(g_edit_hwnd);

    char query[] = "";
    ClearAndDisplayWindowList(g_list_box_hwnd, query);
}

LRESULT MessageWindowProc(
    _In_ HWND hWnd,
    _In_ UINT msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (msg == WM_HOTKEY)
    {
        if (HIWORD(lParam) == c_W_KEY && LOWORD(lParam) == (MOD_WIN | MOD_ALT))
        {
            SendCloseOverlayWindowMessage();

            auto endlife_thread = std::move(g_overlay_window_thread);

            g_overlay_window_thread = std::thread([]
            {
                CreateOverlayWindow();
                RunOverlayWindowThreadLoop();
            });

            // We just created a new window, we should wait for the wind down of the previous one if any.
            if (endlife_thread.joinable())
            {
                endlife_thread.join();
            }

            return 0;
        }
    }
    else if (msg == c_NOTIFY_ICON_MESSAGE)
    {
        if (LOWORD(lParam) == WM_CONTEXTMENU)
        {
            // SetForegroundWindow and PostMessage(WM_NULL) are necessary to before/after calling TrackPopupMenu.
            // Otherwise the menu won't be dimissed by clicking away, or it won't show when right-clicking twice.
            // See Remarks section of https://msdn.microsoft.com/en-us/library/windows/desktop/ms648002(v=vs.85).aspx
            SetForegroundWindow(hWnd);
            TrackPopupMenuEx(
                g_notify_icon_context_menu,
                TPM_LEFTBUTTON | TPM_LEFTALIGN /*uFlags*/,
                GET_X_LPARAM(wParam),
                GET_Y_LPARAM(wParam),
                hWnd,
                nullptr /*lptpm*/);

            PostMessage(hWnd, WM_NULL, 0, 0);

            return 0;
        }
    }
    else if (msg == WM_COMMAND)
    {
        if (HIWORD(wParam) == 0)
        {
            // Notifications from the NotifyIcon menu.
            if (LOWORD(wParam) == c_MENU_ITEM_QUIT)
            {
                // Exit the application. Will exit the message loop.
                PostQuitMessage(0);
            }
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    else
    {
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    return 0;
}

int __stdcall WinMain(
    HINSTANCE hInstance,
    HINSTANCE /*hPrevInstance*/,
    LPSTR /*lpCmdLine*/,
    int /*nCmdShow*/)
{
    WNDCLASSEX message_wnd_class = {};
    message_wnd_class.cbSize = sizeof(WNDCLASSEX);
    message_wnd_class.lpfnWndProc = MessageWindowProc;
    message_wnd_class.hInstance = hInstance;
    message_wnd_class.lpszClassName = "window_switcher_wndclass";

    if (!RegisterClassEx(&message_wnd_class))
    {
        return GetLastError();
    }

    WNDCLASSEX overlay_wnd_class = {};
    overlay_wnd_class.cbSize = sizeof(WNDCLASSEX);
    overlay_wnd_class.lpfnWndProc = OverlayWindowProc;
    overlay_wnd_class.hInstance = hInstance;
    overlay_wnd_class.lpszClassName = c_OVERLAY_WNDCLASS_NAME;

    if (!RegisterClassEx(&overlay_wnd_class))
    {
        return GetLastError();
    }

    WNDCLASSEX mirror_wnd_class = {};
    mirror_wnd_class.cbSize = sizeof(WNDCLASSEX);
    mirror_wnd_class.lpfnWndProc = MirrorWindowProc;
    mirror_wnd_class.hInstance = hInstance;
    mirror_wnd_class.lpszClassName = c_MIRROR_WNDCLASS_NAME;

    if (!RegisterClassEx(&mirror_wnd_class))
    {
        return GetLastError();
    }

    g_notify_icon_context_menu = CreatePopupMenu();
    if (!AppendMenu(
        g_notify_icon_context_menu,
        MF_STRING | MF_ENABLED,
        c_MENU_ITEM_QUIT /*uIDNewItem*/,
        "Quit"))
    {
        return GetLastError();
    }

    HWND message_window = CreateWindowEx(
        0 /*dwExStyle*/,
        message_wnd_class.lpszClassName,
        nullptr,
        0 /*dwStyle*/,
        0 /*x*/,
        0 /*y*/,
        0 /*nWidth*/,
        0 /*nHeight*/,
        HWND_MESSAGE /*hwndParent*/,
        g_notify_icon_context_menu /*menu*/,
        hInstance,
        nullptr /*lparam*/);

    auto notify_icon = CreateNotifyIcon(message_window);

    if (!RegisterHotKey(
        message_window,
        0,
        MOD_WIN | MOD_ALT,
        c_W_KEY /*w key*/))
    {
        return GetLastError();
    }

    RunMainLoop();

    auto endlife_thread = std::move(g_overlay_window_thread);

    // We're about to go down, we need to wait for all threads to exit before we do.
    if (endlife_thread.joinable())
    {
        endlife_thread.join();
    }

    return 0;
}