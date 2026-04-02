#include <napi.h>
#include <windows.h>
#include <map>

static std::map<int, HWND> g_overlays;
static int g_nextId = 1;
static bool g_classRegistered = false;
static const wchar_t* OVERLAY_CLASS = L"TwinkleTrayOverlay";

static void RegisterOverlayClass() {
    if (g_classRegistered) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = OVERLAY_CLASS;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    g_classRegistered = true;
}

// createOverlay(x, y, width, height) -> id
Napi::Number CreateOverlay(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    int x = info[0].As<Napi::Number>().Int32Value();
    int y = info[1].As<Napi::Number>().Int32Value();
    int width = info[2].As<Napi::Number>().Int32Value();
    int height = info[3].As<Napi::Number>().Int32Value();

    RegisterOverlayClass();

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        OVERLAY_CLASS,
        L"",
        WS_POPUP,
        x, y, width, height,
        NULL, NULL, GetModuleHandleW(NULL), NULL
    );

    if (!hwnd) {
        Napi::Error::New(env, "Failed to create overlay window").ThrowAsJavaScriptException();
        return Napi::Number::New(env, -1);
    }

    // Start fully transparent (invisible)
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    int id = g_nextId++;
    g_overlays[id] = hwnd;
    return Napi::Number::New(env, id);
}

// setOverlayOpacity(id, opacity) -> bool  (opacity 0-255)
Napi::Boolean SetOverlayOpacity(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    int id = info[0].As<Napi::Number>().Int32Value();
    int opacity = info[1].As<Napi::Number>().Int32Value();

    auto it = g_overlays.find(id);
    if (it == g_overlays.end()) {
        return Napi::Boolean::New(env, false);
    }

    // Clamp opacity to 0-230 (never fully opaque — ~90% max)
    if (opacity < 0) opacity = 0;
    if (opacity > 230) opacity = 230;

    BOOL result = SetLayeredWindowAttributes(it->second, 0, (BYTE)opacity, LWA_ALPHA);

    // Hide window when fully transparent, show when dimming
    if (opacity == 0) {
        ShowWindow(it->second, SW_HIDE);
    } else if (!IsWindowVisible(it->second)) {
        ShowWindow(it->second, SW_SHOWNOACTIVATE);
    }

    return Napi::Boolean::New(env, result != 0);
}

// repositionOverlay(id, x, y, width, height) -> bool
Napi::Boolean RepositionOverlay(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    int id = info[0].As<Napi::Number>().Int32Value();
    int x = info[1].As<Napi::Number>().Int32Value();
    int y = info[2].As<Napi::Number>().Int32Value();
    int width = info[3].As<Napi::Number>().Int32Value();
    int height = info[4].As<Napi::Number>().Int32Value();

    auto it = g_overlays.find(id);
    if (it == g_overlays.end()) {
        return Napi::Boolean::New(env, false);
    }

    BOOL result = SetWindowPos(it->second, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    return Napi::Boolean::New(env, result != 0);
}

// destroyOverlay(id) -> bool
Napi::Boolean DestroyOverlay(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    int id = info[0].As<Napi::Number>().Int32Value();

    auto it = g_overlays.find(id);
    if (it == g_overlays.end()) {
        return Napi::Boolean::New(env, false);
    }

    DestroyWindow(it->second);
    g_overlays.erase(it);
    return Napi::Boolean::New(env, true);
}

// destroyAllOverlays() -> void
void DestroyAllOverlays(const Napi::CallbackInfo& info) {
    for (auto& pair : g_overlays) {
        DestroyWindow(pair.second);
    }
    g_overlays.clear();
}

// getGdiDeviceName(x, y) -> string
Napi::Value GetGdiDeviceName(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    int x = info[0].As<Napi::Number>().Int32Value();
    int y = info[1].As<Napi::Number>().Int32Value();

    POINT pt = { x, y };
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!hMon) return env.Null();

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(MONITORINFOEXW);
    if (!GetMonitorInfoW(hMon, &mi)) return env.Null();

    char deviceName[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, deviceName, sizeof(deviceName), NULL, NULL);
    return Napi::String::New(env, deviceName);
}

// setGammaRamp(deviceName, brightness) -> bool  (brightness 0-100)
Napi::Boolean SetGammaRamp(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::string deviceName = info[0].As<Napi::String>().Utf8Value();
    int brightness = info[1].As<Napi::Number>().Int32Value();

    // Clamp brightness: 10-100 (never fully black via gamma)
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;

    // Convert UTF-8 to wide string
    int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceName.c_str(), -1, NULL, 0);
    std::wstring wDeviceName(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, deviceName.c_str(), -1, &wDeviceName[0], wlen);

    HDC hdc = CreateDCW(wDeviceName.c_str(), NULL, NULL, NULL);
    if (!hdc) {
        return Napi::Boolean::New(env, false);
    }

    WORD ramp[3][256];
    double factor = brightness / 100.0;
    for (int i = 0; i < 256; i++) {
        WORD value = (WORD)(i * 256 * factor);
        ramp[0][i] = value; // Red
        ramp[1][i] = value; // Green
        ramp[2][i] = value; // Blue
    }

    BOOL result = SetDeviceGammaRamp(hdc, ramp);
    DeleteDC(hdc);
    return Napi::Boolean::New(env, result != 0);
}

// resetGammaRamp(deviceName) -> bool
Napi::Boolean ResetGammaRamp(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::string deviceName = info[0].As<Napi::String>().Utf8Value();

    int wlen = MultiByteToWideChar(CP_UTF8, 0, deviceName.c_str(), -1, NULL, 0);
    std::wstring wDeviceName(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, deviceName.c_str(), -1, &wDeviceName[0], wlen);

    HDC hdc = CreateDCW(wDeviceName.c_str(), NULL, NULL, NULL);
    if (!hdc) {
        return Napi::Boolean::New(env, false);
    }

    WORD ramp[3][256];
    for (int i = 0; i < 256; i++) {
        WORD value = (WORD)(i * 256);
        ramp[0][i] = value;
        ramp[1][i] = value;
        ramp[2][i] = value;
    }

    BOOL result = SetDeviceGammaRamp(hdc, ramp);
    DeleteDC(hdc);
    return Napi::Boolean::New(env, result != 0);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set(Napi::String::New(env, "createOverlay"), Napi::Function::New(env, CreateOverlay));
    exports.Set(Napi::String::New(env, "setOverlayOpacity"), Napi::Function::New(env, SetOverlayOpacity));
    exports.Set(Napi::String::New(env, "repositionOverlay"), Napi::Function::New(env, RepositionOverlay));
    exports.Set(Napi::String::New(env, "destroyOverlay"), Napi::Function::New(env, DestroyOverlay));
    exports.Set(Napi::String::New(env, "destroyAllOverlays"), Napi::Function::New(env, DestroyAllOverlays));
    exports.Set(Napi::String::New(env, "getGdiDeviceName"), Napi::Function::New(env, GetGdiDeviceName));
    exports.Set(Napi::String::New(env, "setGammaRamp"), Napi::Function::New(env, SetGammaRamp));
    exports.Set(Napi::String::New(env, "resetGammaRamp"), Napi::Function::New(env, ResetGammaRamp));
    return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)
