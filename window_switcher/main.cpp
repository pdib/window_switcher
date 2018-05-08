#include <Windows.h>
#include <Windowsx.h>
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

constexpr unsigned int c_NOTIFY_ICON_MESSAGE = WM_APP + 0x0001;
constexpr unsigned int c_MENU_ITEM_QUIT = 0x0001;

HMENU g_notify_icon_context_menu = nullptr;
HWND g_edit_hwnd = nullptr;

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
    auto style = GetWindowLong(hwnd, GWL_STYLE);
    if (IsWindowVisible(hwnd) && (style & WS_GROUP))
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
        std::transform(begin(window_title), end(window_title), begin(window_title), [](unsigned char c) { return std::tolower(c); });
        std::transform(begin(process_name), end(process_name), begin(process_name), [](unsigned char c) { return std::tolower(c); });
        std::transform(begin(query), end(query), begin(query), [](unsigned char c) { return std::tolower(c); });

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
    std::string token = strtok_s(wholeQuery, " ", &next_token);
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

// Main action for the window_switcher program:
// - List all running windows
// - Filter the list based on user query
// - Switch to the selected window
void InvokeWindowSwitcher()
{
    auto hwnds = GetVisibleWindows();
    auto wpis = PopulateWindowInformation(hwnds);

    std::vector<size_t> matching_indices;
    while (matching_indices.empty())
    {
        char query[100];
        std::cin >> query;

        QueryWindows(query, wpis, matching_indices);
    }

    for (int index = 0; index < matching_indices.size(); ++index)
    {
        auto wpi = wpis[matching_indices[index]];
        std::cout
            << index
            << ": ["
            << wpi.pid
            << "] "
            << wpi.window_title
            << " - "
            << wpi.process_name
            << std::endl;
    }

    int val;
    std::cin >> val;
    SwitchToThisWindow(hwnds[matching_indices[val]], false);
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

void RunThreadMessageLoop()
{
    bool isRunning = true;
    while (isRunning)
    {
        MSG message;
        // Deliberately listen to all messages destined to the thread. Not only a specific window's messages.
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

LRESULT MessageWindowProc(
    _In_ HWND hWnd,
    _In_ UINT msg,
    _In_ WPARAM wParam,
    _In_ LPARAM lParam)
{
    if (msg == WM_HOTKEY)
    {
        if (HIWORD(lParam) == 0x5A && LOWORD(lParam) == (MOD_WIN | MOD_ALT))
        {
            if (g_edit_hwnd)
            {
                DestroyWindow(g_edit_hwnd);
            }

            g_edit_hwnd = CreateWindow(
                "Edit",
                "",
                WS_VISIBLE | ES_LEFT | WS_BORDER | WS_POPUPWINDOW,
                100,
                150,
                350,
                20,
                hWnd,
                nullptr,
                nullptr,
                nullptr);

            SetFocus(g_edit_hwnd);
            SetForegroundWindow(g_edit_hwnd);

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
        if (lParam != 0 && (HWND)lParam == g_edit_hwnd)
        {
            // Edit control notifications.
            if (HIWORD(wParam) == EN_KILLFOCUS)
            {
                DestroyWindow(g_edit_hwnd);
                g_edit_hwnd = nullptr;
            }
        }
        else if (HIWORD(wParam) == 0)
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
    HINSTANCE hPrevInstance,
    LPSTR lpCmdLine,
    int nCmdShow)
{
    WNDCLASSEX wnd_class = {};
    wnd_class.cbSize = sizeof(WNDCLASSEX);
    wnd_class.lpfnWndProc = MessageWindowProc;
    wnd_class.hInstance = hInstance;
    wnd_class.lpszClassName = "window_switcher_wndclass";

    if (!RegisterClassEx(&wnd_class))
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
        wnd_class.lpszClassName,
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
        0x5A /*w key*/))
    {
        return GetLastError();
    }

    RunThreadMessageLoop();

    return 0;
}