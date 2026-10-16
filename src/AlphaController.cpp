// AlphaController: maps an XInput (Xbox-compatible) gamepad to the keyboard and
// mouse input that Cube World Alpha expects.
//
// The same source builds two ways:
//   * AlphaController.exe - a standalone tray application.
//   * AlphaController.dll - (ALPHA_CONTROLLER_DLL) loaded by the Alpha mod loader;
//     it starts the same tray application on a background thread inside Cube.exe.
//
// Every 8 ms the mapper polls the first connected controller and, while the
// game window has focus, turns buttons into key/mouse-button presses and the
// right stick into relative mouse movement.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <xinput.h>
#include <cmath>
#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>
#include <array>
#include <cstdio>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static HMODULE ownModule = nullptr; // Set by DllMain in the DLL build.

static constexpr UINT POLL_TIMER_ID = 1;
static constexpr UINT POLL_INTERVAL_MS = 8;
static constexpr UINT TRAY_MESSAGE = WM_APP + 1;

// Mouse buttons share the "key code" space with virtual-key codes (0-255).
static constexpr int MOUSE_LEFT = 256, MOUSE_RIGHT = 257, MOUSE_MIDDLE = 258;
static constexpr int INPUT_CODE_COUNT = 259; // Virtual keys plus the three mouse buttons.

// Results of parsing a binding name.
static constexpr int BINDING_NONE = -1;    // "NONE": the control is deliberately unbound.
static constexpr int BINDING_INVALID = -2; // Unknown text: the INI file is rejected.

static constexpr double MOVE_STICK_THRESHOLD = .25; // Left-stick deflection that presses WASD.
static constexpr BYTE TRIGGER_THRESHOLD = 40; // Trigger travel (0-255) that counts as pressed.
static constexpr ULONGLONG GAME_EXIT_GRACE_MS = 2000; // --auto-stop waits this long after exit.

static const wchar_t *const GAME_EXECUTABLES[] = {L"Cube.exe", L"Cube-SteamWater-Preview.exe"};

// One entry per bindable controller input. The order matters: index 0 (A) and 1 (B)
// become mouse clicks in menu mode, and LT/RT are read from the analog triggers.
struct ControlInfo {
    const wchar_t *iniName; // Key in the [Bindings] section of AlphaController.ini.
    WORD buttonMask;        // XInput button bit (0 for the analog triggers).
    const wchar_t *defaultBinding;
};
static const ControlInfo CONTROLS[] = {
    {L"A", XINPUT_GAMEPAD_A, L"SPACE"},
    {L"B", XINPUT_GAMEPAD_B, L"MIDDLE_MOUSE"},
    {L"X", XINPUT_GAMEPAD_X, L"E"},
    {L"Y", XINPUT_GAMEPAD_Y, L"R"},
    {L"LB", XINPUT_GAMEPAD_LEFT_SHOULDER, L"SHIFT"},
    {L"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER, L"Q"},
    {L"Back", XINPUT_GAMEPAD_BACK, L"TAB"},
    {L"Start", XINPUT_GAMEPAD_START, L"ESC"},
    {L"L3", XINPUT_GAMEPAD_LEFT_THUMB, L"CTRL"},
    {L"DPadUp", XINPUT_GAMEPAD_DPAD_UP, L"F"},
    {L"DPadDown", XINPUT_GAMEPAD_DPAD_DOWN, L"C"},
    {L"DPadLeft", XINPUT_GAMEPAD_DPAD_LEFT, L"B"},
    {L"DPadRight", XINPUT_GAMEPAD_DPAD_RIGHT, L"M"},
    {L"LT", 0, L"RIGHT_MOUSE"},
    {L"RT", 0, L"LEFT_MOUSE"},
};
static constexpr int CONTROL_COUNT = 15;
static constexpr int CONTROL_A = 0, CONTROL_B = 1, CONTROL_LT = 13, CONTROL_RT = 14;
static_assert(sizeof(CONTROLS) / sizeof(CONTROLS[0]) == CONTROL_COUNT, "control table size");

// ---------------------------------------------------------------------------
// Pure helpers (also exercised by --self-test)
// ---------------------------------------------------------------------------

struct Stick {
    double x, y;
};

// Converts a raw thumbstick reading into a -1..1 vector with a radial dead zone.
// The output magnitude is rescaled so it starts at 0 just outside the dead zone.
static Stick stick(SHORT rawX, SHORT rawY, double deadZone) {
    double nx = rawX / 32768.0, ny = rawY / 32768.0;
    double length = std::sqrt(nx * nx + ny * ny);
    if (length <= deadZone)
        return {0, 0};
    double scaled = (std::min(length, 1.0) - deadZone) / (1.0 - deadZone);
    return {nx / length * scaled, ny / length * scaled};
}

// Parses an (upper-case) binding name into a virtual-key or mouse code.
// Returns BINDING_NONE for "NONE" and BINDING_INVALID for anything unrecognised.
static int key(const std::wstring &text) {
    static const struct {
        const wchar_t *name;
        int code;
    } NAMED_KEYS[] = {
        {L"LEFT_MOUSE", MOUSE_LEFT},
        {L"RIGHT_MOUSE", MOUSE_RIGHT},
        {L"MIDDLE_MOUSE", MOUSE_MIDDLE},
        {L"SPACE", VK_SPACE},
        {L"SHIFT", VK_SHIFT},
        {L"CTRL", VK_CONTROL},
        {L"ALT", VK_MENU},
        {L"TAB", VK_TAB},
        {L"ESC", VK_ESCAPE},
        {L"ENTER", VK_RETURN},
        {L"NONE", BINDING_NONE},
    };
    for (const auto &named : NAMED_KEYS)
        if (text == named.name)
            return named.code;
    // Single letters and digits map directly to their virtual-key codes.
    if (text.size() == 1 &&
        ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'0' && text[0] <= L'9')))
        return text[0];
    return BINDING_INVALID;
}

// ---------------------------------------------------------------------------
// Mapper: owns the tray icon, settings and the per-tick input translation
// ---------------------------------------------------------------------------

struct Mapper {
    HWND window{};
    NOTIFYICONDATAW tray{};
    HMODULE xinput{};
    DWORD(WINAPI *getState)(DWORD, XINPUT_STATE *){};

    std::wstring directory;     // Folder containing this module.
    std::wstring ini;           // Full path of AlphaController.ini.
    std::wstring gameDirectory; // Parent of `directory`; where Cube.exe lives.
    std::wstring gamePaths[2];  // Full paths of the accepted game executables.

    std::array<int, CONTROL_COUNT> bindings{}; // Input code per CONTROLS entry.
    std::array<bool, INPUT_CODE_COUNT> held{}; // Codes currently pressed by the mapper.

    // Settings (defaults match the shipped INI file).
    double leftDead = .24, rightDead = .18, cameraSpeed = 1000, menuSpeed = 650;
    // Sub-pixel mouse movement carried over between ticks.
    double fx = 0, fy = 0;

    WORD previous = 0; // Buttons seen on the previous tick, for edge detection.
    bool menu = false; // Menu mode: A/B click the mouse and the left stick is ignored.
    bool connected = false, focused = false;
    bool hadGame = false;  // The game window has been focused at least once.
    bool autoStop = false; // --auto-stop: exit once the game process has ended.
    ULONGLONG last = 0, missingSince = 0;
    DWORD gamePid = 0;

    // Sends one key or mouse-button transition through SendInput.
    bool inject(int code, bool down) {
        INPUT input{};
        if (code >= MOUSE_LEFT) {
            input.type = INPUT_MOUSE;
            static const DWORD pressFlags[] = {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN,
                                               MOUSEEVENTF_MIDDLEDOWN};
            static const DWORD releaseFlags[] = {MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP,
                                                 MOUSEEVENTF_MIDDLEUP};
            input.mi.dwFlags =
                down ? pressFlags[code - MOUSE_LEFT] : releaseFlags[code - MOUSE_LEFT];
        } else {
            // Scan codes work with games that read raw keyboard input.
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = (WORD)MapVirtualKeyW(code, MAPVK_VK_TO_VSC);
            input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
        }
        return SendInput(1, &input, sizeof(input)) == 1;
    }

    // Releases everything the mapper is holding and drops pending mouse motion.
    void release() {
        for (int code = 0; code < (int)held.size(); ++code)
            if (held[code]) {
                inject(code, false);
                held[code] = false;
            }
        fx = fy = 0;
    }

    static std::wstring pathFor(DWORD pid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            return L"";
        wchar_t path[32768];
        DWORD length = 32768;
        bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
        CloseHandle(process);
        return ok ? std::wstring(path, length) : L"";
    }

    bool matches(const std::wstring &path) const {
        for (const std::wstring &gamePath : gamePaths)
            if (!_wcsicmp(path.c_str(), gamePath.c_str()))
                return true;
        return false;
    }

    // True when the foreground window belongs to the game next to this mod.
    bool foreground() {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        bool result = pid && matches(pathFor(pid));
        if (result) {
            gamePid = pid;
            hadGame = true;
            missingSince = 0;
        }
        return result;
    }

    void status() {
        std::wstring text = L"Alpha controller: ";
        text += !connected ? L"Xbox disconnected"
                : !focused ? L"waiting for alpha"
                : menu     ? L"menu mouse mode"
                           : L"gameplay mode";
        wcsncpy_s(tray.szTip, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &tray);
    }

    void toggleMenuMode() {
        release();
        menu = !menu;
        status();
    }

    // Reads AlphaController.ini and loads XInput. Returns false on any invalid setting.
    bool load() {
        wchar_t file[32768];
        GetModuleFileNameW(ownModule, file, 32768);
        directory = file;
        directory = directory.substr(0, directory.find_last_of(L"\\/"));
        gameDirectory = directory.substr(0, directory.find_last_of(L"\\/"));
        for (int i = 0; i < 2; ++i)
            gamePaths[i] = gameDirectory + L"\\" + GAME_EXECUTABLES[i];
#ifdef ALPHA_CONTROLLER_DLL
        ini = gameDirectory + L"\\controller\\AlphaController.ini";
#else
        ini = directory + L"\\AlphaController.ini";
#endif
        wchar_t value[256];
        for (int i = 0; i < CONTROL_COUNT; ++i) {
            GetPrivateProfileStringW(L"Bindings", CONTROLS[i].iniName, CONTROLS[i].defaultBinding,
                                     value, 256, ini.c_str());
            CharUpperBuffW(value, (DWORD)wcslen(value));
            bindings[i] = key(value);
            if (bindings[i] == BINDING_INVALID)
                return false;
        }
        auto setting = [&](const wchar_t *name, const wchar_t *fallback) {
            GetPrivateProfileStringW(L"Settings", name, fallback, value, 256, ini.c_str());
            return wcstod(value, nullptr);
        };
        leftDead = setting(L"LeftDeadzone", L"0.24");
        rightDead = setting(L"RightDeadzone", L"0.18");
        cameraSpeed = setting(L"CameraPixelsPerSecond", L"1000");
        menuSpeed = setting(L"MenuPixelsPerSecond", L"650");
        bool deadZonesValid = leftDead >= 0 && leftDead < .95 && rightDead >= 0 && rightDead < .95;
        bool speedsValid =
            cameraSpeed > 0 && cameraSpeed <= 10000 && menuSpeed > 0 && menuSpeed <= 10000;
        if (!(deadZonesValid && speedsValid))
            return false;
        return loadXInput();
    }

    bool loadXInput() {
        for (const wchar_t *name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            xinput = LoadLibraryW(name);
            if (!xinput)
                continue;
            getState = (decltype(getState))GetProcAddress(xinput, "XInputGetState");
            if (getState)
                return true;
            FreeLibrary(xinput);
            xinput = nullptr;
        }
        return false;
    }

    // --auto-stop: close the mapper a little after the game process has exited.
    // Returns true when the window was destroyed.
    bool stopIfGameEnded(ULONGLONG now, bool gameFocused) {
        if (!(autoStop && hadGame && !gameFocused && gamePid))
            return false;
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
        bool ended = !process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        if (process)
            CloseHandle(process);
        if (!ended)
            return false;
        if (!missingSince)
            missingSince = now;
        if (now - missingSince <= GAME_EXIT_GRACE_MS)
            return false;
        DestroyWindow(window);
        return true;
    }

    // Presses/releases keys so that exactly the codes wanted this tick are held.
    void updateButtons(const XINPUT_GAMEPAD &gp, const Stick &left) {
        std::array<bool, INPUT_CODE_COUNT> desired{};
        auto want = [&](int code, bool down) {
            if (code >= 0 && down)
                desired[code] = true;
        };
        if (!menu) {
            // Digital WASD movement from the left stick.
            want('A', left.x < -MOVE_STICK_THRESHOLD);
            want('D', left.x > MOVE_STICK_THRESHOLD);
            want('W', left.y > MOVE_STICK_THRESHOLD);
            want('S', left.y < -MOVE_STICK_THRESHOLD);
        }
        for (int i = 0; i < CONTROL_COUNT; ++i) {
            bool down = i == CONTROL_LT   ? gp.bLeftTrigger > TRIGGER_THRESHOLD
                        : i == CONTROL_RT ? gp.bRightTrigger > TRIGGER_THRESHOLD
                                          : (gp.wButtons & CONTROLS[i].buttonMask) != 0;
            int binding = menu && i == CONTROL_A   ? MOUSE_LEFT
                          : menu && i == CONTROL_B ? MOUSE_RIGHT
                                                   : bindings[i];
            want(binding, down);
        }
        for (int code = 0; code < (int)held.size(); ++code)
            if (held[code] != desired[code] && inject(code, desired[code]))
                held[code] = desired[code];
    }

    // Right stick moves the mouse; speed grows with the square of the deflection.
    // Fractional pixels are carried over so slow movement still accumulates.
    void moveMouse(const XINPUT_GAMEPAD &gp, double dt) {
        Stick right = stick(gp.sThumbRX, gp.sThumbRY, rightDead);
        double magnitude = std::sqrt(right.x * right.x + right.y * right.y);
        double speed = menu ? menuSpeed : cameraSpeed;
        fx += right.x * magnitude * speed * dt;
        fy -= right.y * magnitude * speed * dt;
        LONG dx = (LONG)fx, dy = (LONG)fy;
        fx -= dx;
        fy -= dy;
        if (dx || dy) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dx = dx;
            input.mi.dy = dy;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            SendInput(1, &input, sizeof(input));
        }
    }

    // Called from the poll timer.
    void tick() {
        ULONGLONG now = GetTickCount64();
        double dt = last ? std::min((now - last) / 1000.0, .05) : .008;
        last = now;

        // Use the first connected controller slot.
        XINPUT_STATE state{};
        bool found = false;
        for (DWORD i = 0; i < 4; ++i)
            if (getState(i, &state) == ERROR_SUCCESS) {
                found = true;
                break;
            }
        bool active = foreground();
        if (connected != found || focused != active) {
            connected = found;
            focused = active;
            status();
        }
        if (stopIfGameEnded(now, active))
            return;
        if (!found || !active) {
            release();
            previous = found ? state.Gamepad.wButtons : 0;
            return;
        }

        const XINPUT_GAMEPAD &gp = state.Gamepad;
        // R3 toggles menu mode on the press edge.
        bool toggle =
            (gp.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) && !(previous & XINPUT_GAMEPAD_RIGHT_THUMB);
        if (toggle)
            toggleMenuMode();
        previous = gp.wButtons;

        updateButtons(gp, stick(gp.sThumbLX, gp.sThumbLY, leftDead));
        moveMouse(gp, dt);
    }
};

// ---------------------------------------------------------------------------
// Window / tray plumbing
// ---------------------------------------------------------------------------

static Mapper *mapper = nullptr;

enum TrayCommand { TRAY_OPEN_BINDINGS = 1, TRAY_TOGGLE_MENU = 2, TRAY_EXIT = 3 };

static void showTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, TRAY_OPEN_BINDINGS, L"Open bindings");
    AppendMenuW(menu, MF_STRING, TRAY_TOGGLE_MENU, L"Toggle menu mouse mode (R3)");
    AppendMenuW(menu, MF_STRING, TRAY_EXIT, L"Exit controller mapper");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    if (selected == TRAY_OPEN_BINDINGS)
        ShellExecuteW(nullptr, L"open", mapper->ini.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (selected == TRAY_TOGGLE_MENU)
        mapper->toggleMenuMode();
    if (selected == TRAY_EXIT)
        DestroyWindow(hwnd);
}

static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_TIMER && mapper) {
        mapper->tick();
        return 0;
    }
    if (msg == TRAY_MESSAGE && mapper && (l == WM_RBUTTONUP || l == WM_LBUTTONUP)) {
        showTrayMenu(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        if (mapper) {
            mapper->release();
            Shell_NotifyIconW(NIM_DELETE, &mapper->tray);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

// Returns 0 when all checks pass, 1 otherwise.
static int selfTest() {
    int failures = 0;
    auto check = [&](bool test) {
        if (!test)
            ++failures;
    };
    check(stick(0, 0, .24).x == 0);
    check(stick(1000, -1000, .24).y == 0);
    auto diagonal = stick(32767, 32767, .24);
    check(std::abs(std::sqrt(diagonal.x * diagonal.x + diagonal.y * diagonal.y) - 1) < .0001);
    check(stick(-32768, 0, .24).x == -1);
    check(key(L"MIDDLE_MOUSE") == MOUSE_MIDDLE);
    check(key(L"Q") == 'Q');
    check(key(L"NONE") == BINDING_NONE);
    check(key(L"INVALID") == BINDING_INVALID);
    return failures ? 1 : 0;
}

// Arguments: --self-test (run checks and exit), --auto-stop (exit after the game closes).
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR args, int) {
    if (wcsstr(args, L"--self-test"))
        return selfTest();
    // Only one mapper may run at a time (standalone exe or DLL).
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\CubeAlphaXboxMapper");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    Mapper app;
    mapper = &app;
    app.autoStop = wcsstr(args, L"--auto-stop") != nullptr;
    if (!app.load()) {
        MessageBoxW(nullptr, L"Check AlphaController.ini settings and Windows XInput availability.",
                    L"Alpha controller could not start", MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }
    WNDCLASSW cls{};
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = L"CubeAlphaControllerMapper";
    RegisterClassW(&cls);
    app.window = CreateWindowW(cls.lpszClassName, L"Alpha Xbox Controller", 0, 0, 0, 0, 0, nullptr,
                               nullptr, instance, nullptr);
    app.tray.cbSize = sizeof(app.tray);
    app.tray.hWnd = app.window;
    app.tray.uID = 1;
    app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    app.tray.uCallbackMessage = TRAY_MESSAGE;
    app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(app.tray.szTip, L"Alpha controller: waiting for game");
    Shell_NotifyIconW(NIM_ADD, &app.tray);
    SetTimer(app.window, POLL_TIMER_ID, POLL_INTERVAL_MS, nullptr);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    FreeLibrary(app.xinput);
    CloseHandle(mutex);
    return 0;
}

// ---------------------------------------------------------------------------
// DLL entry points (mod-loader build)
// ---------------------------------------------------------------------------

#ifdef ALPHA_CONTROLLER_DLL
extern "C" __declspec(dllexport) int __cdecl ControllerSelfTest() {
    return selfTest();
}

static DWORD WINAPI controllerThread(LPVOID parameter) {
    HMODULE module = (HMODULE)parameter;
    // Take an extra reference so the DLL stays loaded until this thread exits.
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&controllerThread, &module);
    wchar_t empty[] = L"";
    DWORD result = wWinMain(module, nullptr, empty, SW_HIDE);
    FreeLibraryAndExitThread(module, result);
}

static bool isGameExecutable(const wchar_t *fileName) {
    for (const wchar_t *game : GAME_EXECUTABLES)
        if (!_wcsicmp(fileName, game))
            return true;
    return false;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t exe[32768];
        GetModuleFileNameW(nullptr, exe, 32768);
        const wchar_t *name = wcsrchr(exe, L'\\');
        name = name ? name + 1 : exe;
        if (!isGameExecutable(name))
            return TRUE; // Loaded into some other process: stay inert.
        ownModule = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, controllerThread, module, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
#endif
