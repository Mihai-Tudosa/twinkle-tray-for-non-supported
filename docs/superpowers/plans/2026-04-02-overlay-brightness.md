# Overlay Brightness for Unsupported Monitors — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add software-based brightness dimming (native Win32 overlay + gamma ramp fallback) for monitors that lack DDC/CI or WMI support, and allow any monitor to opt into overlay mode.

**Architecture:** A new C++ native module (`windows_overlay.cc`) in `tt-windows-utils` provides Win32 overlay window management and gamma ramp control. `Monitors.js` gains an `"overlay"` monitor type and handles overlay brightness via IPC to the main process. The main Electron process (`electron.js`) manages overlay lifecycle (create/move/destroy) since it has access to screen geometry. The React UI treats overlay monitors identically to DDC/CI monitors with a small icon indicator.

**Tech Stack:** C++ (Win32 API, NAPI), JavaScript (Node.js, Electron, React)

---

### File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `src/modules/tt-windows-utils/windows_overlay.cc` | Create | Native Win32 overlay window creation/destruction/opacity + gamma ramp |
| `src/modules/tt-windows-utils/binding.gyp` | Modify | Add `windows_overlay` build target |
| `src/modules/tt-windows-utils/index.js` | Modify | Export new `Overlay` module |
| `src/Monitors.js` | Modify | Add `"overlay"` type assignment, overlay brightness branch in `setBrightness()` |
| `src/electron.js` | Modify | Overlay lifecycle management (create/reposition/destroy), fullscreen detection polling, new IPC handler for overlay opacity |
| `src/components/BrightnessPanel.jsx` | Modify | Include `"overlay"` type in monitor filtering, add overlay icon |
| `src/components/SettingsWindow.jsx` | Modify | Add brightness method dropdown, show overlay monitors, include `"overlay"` in type checks |
| `src/components/MonitorInfo.jsx` | Modify | Show "Overlay" in communication type, display overlay brightness info |

---

### Task 1: Native Win32 Overlay Module (`windows_overlay.cc`)

**Files:**
- Create: `src/modules/tt-windows-utils/windows_overlay.cc`
- Modify: `src/modules/tt-windows-utils/binding.gyp`
- Modify: `src/modules/tt-windows-utils/index.js`

- [ ] **Step 1: Create `windows_overlay.cc` with overlay window functions**

Create `src/modules/tt-windows-utils/windows_overlay.cc`:

```cpp
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
    exports.Set(Napi::String::New(env, "setGammaRamp"), Napi::Function::New(env, SetGammaRamp));
    exports.Set(Napi::String::New(env, "resetGammaRamp"), Napi::Function::New(env, ResetGammaRamp));
    return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, Init)
```

- [ ] **Step 2: Add build target to `binding.gyp`**

Add this entry to the `"targets"` array in `src/modules/tt-windows-utils/binding.gyp`, after the last existing target:

```json
{
  "target_name": "windows_overlay",
  "cflags!": [ ],
  "cflags_cc!": [ ],
  "sources": [ "windows_overlay.cc" ],
  "include_dirs": [
    "<!@(node -p \"require('node-addon-api').include\")"
  ],
  "msvs_settings": {
    "VCCLCompilerTool": { "ExceptionHandling": 1, "AdditionalOptions": [ "-std:c++17" ] }
  },
  "defines": [ "NAPI_CPP_EXCEPTIONS" ],
  "libraries": [ "gdi32.lib" ]
}
```

Note: `gdi32.lib` is needed for `CreateDCW`, `SetDeviceGammaRamp`, `DeleteDC`.

- [ ] **Step 3: Export the Overlay module from `index.js`**

Add to `src/modules/tt-windows-utils/index.js`:

At the top, add the require:
```javascript
const Overlay = require("bindings")("windows_overlay");
```

In the `module.exports` object, add:
```javascript
Overlay: {
    createOverlay: Overlay.createOverlay,
    setOverlayOpacity: Overlay.setOverlayOpacity,
    repositionOverlay: Overlay.repositionOverlay,
    destroyOverlay: Overlay.destroyOverlay,
    destroyAllOverlays: Overlay.destroyAllOverlays,
    setGammaRamp: Overlay.setGammaRamp,
    resetGammaRamp: Overlay.resetGammaRamp,
    getGdiDeviceName: Overlay.getGdiDeviceName
}
```

- [ ] **Step 4: Build and verify the module compiles**

Run from `src/modules/tt-windows-utils/`:
```bash
cd src/modules/tt-windows-utils && npm run rebuild
```

Expected: Build succeeds with `windows_overlay.node` in the build directory alongside existing `.node` files.

- [ ] **Step 5: Commit**

```bash
git add src/modules/tt-windows-utils/windows_overlay.cc src/modules/tt-windows-utils/binding.gyp src/modules/tt-windows-utils/index.js
git commit -m "feat: add native Win32 overlay and gamma ramp module"
```

---

### Task 2: Add `"overlay"` Type to Monitor Detection (`Monitors.js`)

**Files:**
- Modify: `src/Monitors.js`

- [ ] **Step 1: Add overlay type assignment in `getAllMonitors()`**

In `src/Monitors.js`, find the section at line ~430 that fixes names/num (the `// Finally, fix names/num` block). Insert the following block **before** that section (after line 428, the `hideClosedLid` block):

```javascript
    // Assign overlay type to unsupported monitors (or user-overridden ones)
    for (const hwid2 in foundMonitors) {
        const monitor = foundMonitors[hwid2]
        const overlayOverride = settings?.overlayDisplays?.[hwid2]
        if (overlayOverride === true) {
            // User forced overlay mode for this monitor
            monitor.type = "overlay"
        } else if (overlayOverride === false) {
            // User explicitly disabled overlay (hardware only) — keep current type
        } else if (monitor.type === "none") {
            // Auto: unsupported monitors default to overlay
            monitor.type = "overlay"
        }
    }
```

- [ ] **Step 2: Add overlay branch in `setBrightness()`**

In `src/Monitors.js`, find `function setBrightness(brightness, id)` at line 926. The function has branches for `studio-display`, high-level brightness, and default VCP. After the `studio-display` check (line 935) and before the high-level brightness check, add an overlay branch.

Replace the entire if/else chain inside the `if(monitor)` block (lines 934-944) with:

```javascript
                if (monitor.type == "overlay") {
                    // Overlay brightness is handled by the main process
                    // Convert brightness 0-100 to opacity 0-230 (never fully opaque)
                    const opacity = Math.round((1 - brightness / 100) * 230)
                    process.send({
                        type: "overlayBrightness",
                        id: monitor.id,
                        key: monitor.key,
                        opacity: opacity,
                        brightness: brightness
                    })
                } else if (monitor.type == "studio-display") {
                    setStudioDisplayBrightness(monitor.serial, brightness)
                } else if(!settings.disableHighLevel && monitor.highLevelSupported?.brightness && !hasCustomBrightnessVCP) {
                    setHighLevelBrightness(monitor.hwid.join("#"), brightness)
                } else {
                    setVCP(monitor.hwid.join("#"), monitor.brightnessType, brightness)
                }
```

- [ ] **Step 3: Include overlay type in the WMI-less brightness path**

In the same `setBrightness()` function, the `else` branch (line 948) handles WMI monitors without an ID. No changes needed here — overlay monitors always have an ID.

- [ ] **Step 4: Commit**

```bash
git add src/Monitors.js
git commit -m "feat: add overlay monitor type and brightness routing in Monitors.js"
```

---

### Task 3: Overlay Lifecycle Management in Electron Main Process (`electron.js`)

**Files:**
- Modify: `src/electron.js`

- [ ] **Step 1: Import the Overlay module and add overlay state tracking**

Near the top of `src/electron.js`, after the existing requires (around line 20-30), add:

```javascript
let Overlay
try {
    Overlay = require("tt-windows-utils").Overlay
} catch (e) {
    console.log("Overlay module not available:", e)
}
```

After the `let monitors = {}` declaration (search for it, it's in the early variable declarations area), add:

```javascript
let overlayHandles = {}       // { monitorKey: overlayId }
let overlayGammaActive = {}   // { monitorKey: true } when gamma ramp is active
let fullscreenCheckInterval = null
```

- [ ] **Step 2: Add overlay creation/management functions**

Add these functions after the overlay state variables:

```javascript
function createOverlayForMonitor(monitor) {
    if (!Overlay || !monitor?.bounds) return false
    if (overlayHandles[monitor.key]) return overlayHandles[monitor.key]
    try {
        const b = monitor.bounds
        const id = Overlay.createOverlay(b.left, b.top, b.right - b.left, b.bottom - b.top)
        if (id > 0) {
            overlayHandles[monitor.key] = id
            // Restore last-known brightness as opacity
            const brightness = monitor.brightness ?? 100
            const opacity = Math.round((1 - brightness / 100) * 230)
            Overlay.setOverlayOpacity(id, opacity)
            console.log(`Overlay created for ${monitor.key} (id: ${id})`)
        }
        return id
    } catch (e) {
        console.log(`Failed to create overlay for ${monitor.key}:`, e)
        return false
    }
}

function destroyOverlayForMonitor(monitorKey) {
    if (!Overlay) return
    const id = overlayHandles[monitorKey]
    if (id) {
        Overlay.destroyOverlay(id)
        delete overlayHandles[monitorKey]
        console.log(`Overlay destroyed for ${monitorKey}`)
    }
    // Reset gamma if it was active
    if (overlayGammaActive[monitorKey]) {
        const monitor = monitors[monitorKey]
        if (monitor?.bounds) {
            try {
                const gdiName = Overlay.getGdiDeviceName(monitor.bounds.left, monitor.bounds.top)
                if (gdiName) Overlay.resetGammaRamp(gdiName)
            } catch(e) {}
        }
        delete overlayGammaActive[monitorKey]
    }
}

function destroyAllOverlayWindows() {
    if (!Overlay) return
    try {
        Overlay.destroyAllOverlays()
    } catch(e) {}
    // Reset all gamma ramps
    for (const key in overlayGammaActive) {
        const monitor = monitors[key]
        if (monitor?.bounds) {
            try {
                const gdiName = Overlay.getGdiDeviceName(monitor.bounds.left, monitor.bounds.top)
                if (gdiName) Overlay.resetGammaRamp(gdiName)
            } catch(e) {}
        }
    }
    overlayHandles = {}
    overlayGammaActive = {}
}

function repositionAllOverlays() {
    if (!Overlay) return
    for (const key in overlayHandles) {
        const monitor = monitors[key]
        if (monitor?.bounds) {
            const b = monitor.bounds
            Overlay.repositionOverlay(overlayHandles[key], b.left, b.top, b.right - b.left, b.bottom - b.top)
        }
    }
}

function syncOverlaysWithMonitors() {
    if (!Overlay) return
    // Create overlays for new overlay monitors
    for (const key in monitors) {
        if (monitors[key].type === "overlay" && !overlayHandles[key]) {
            createOverlayForMonitor(monitors[key])
        }
    }
    // Destroy overlays for monitors no longer in overlay mode
    for (const key in overlayHandles) {
        if (!monitors[key] || monitors[key].type !== "overlay") {
            destroyOverlayForMonitor(key)
        }
    }
    // Reposition existing overlays
    repositionAllOverlays()
}
```

- [ ] **Step 3: Handle overlay brightness IPC from Monitors thread**

Find the `monitorsThreadReal.on("message", (data) => {` handler inside `startMonitorThread()` (around line 182). Add a handler for the new `overlayBrightness` message type. Inside the `if (data?.type)` block, add:

```javascript
      if (data.type === "overlayBrightness") {
          const key = data.key
          if (Overlay && overlayHandles[key] && !overlayGammaActive[key]) {
              Overlay.setOverlayOpacity(overlayHandles[key], data.opacity)
          } else if (Overlay && overlayGammaActive[key]) {
              // Exclusive fullscreen active — use gamma ramp instead
              const monitor = monitors[key]
              if (monitor?.bounds) {
                  const gdiName = Overlay.getGdiDeviceName(monitor.bounds.left, monitor.bounds.top)
                  if (gdiName) {
                      const brightness = Math.max(10, data.brightness)
                      Overlay.setGammaRamp(gdiName, brightness)
                  }
              }
          }
      }
```

- [ ] **Step 4: Create overlays after monitor refresh**

Find where `refreshMonitors` data is received. Search for `monitorsEventEmitter.emit(data.type, data)` — this is where all monitor thread messages are dispatched. There's a listener for `refreshMonitors` that updates the `monitors` variable. Find where `monitors` is assigned from the refresh result (search for `monitors-updated` sends near the refresh handler). After `monitors` is updated and `sendToAllWindows('monitors-updated', monitors)` is called, add:

```javascript
syncOverlaysWithMonitors()
```

This needs to be added in every location where monitors are updated and `sendToAllWindows('monitors-updated', monitors)` is called. Search for all instances of `sendToAllWindows('monitors-updated'` and add `syncOverlaysWithMonitors()` after each one.

- [ ] **Step 5: Add fullscreen detection polling**

Add after the overlay management functions:

```javascript
function startFullscreenDetection() {
    if (fullscreenCheckInterval) return
    fullscreenCheckInterval = setInterval(() => {
        if (!Overlay || Object.keys(overlayHandles).length === 0) return
        try {
            const { WindowUtils } = require("tt-windows-utils")
            const fgHwnd = WindowUtils.getForegroundWindow()
            if (!fgHwnd) return
            const isFullscreen = WindowUtils.getWindowFullscreen(fgHwnd)

            if (isFullscreen) {
                // Determine which monitor the fullscreen window is on
                // Use the window position to match against monitor bounds
                const fgPos = WindowUtils.getWindowPos(fgHwnd)
                for (const key in overlayHandles) {
                    const monitor = monitors[key]
                    if (!monitor?.bounds) continue
                    const b = monitor.bounds
                    // Check if the fullscreen window covers this monitor
                    if (fgPos.left === b.left && fgPos.top === b.top &&
                        fgPos.width === (b.right - b.left) && fgPos.height === (b.bottom - b.top)) {
                        // Fullscreen on this monitor — switch to gamma ramp
                        if (!overlayGammaActive[key]) {
                            overlayGammaActive[key] = true
                            Overlay.setOverlayOpacity(overlayHandles[key], 0) // Hide overlay
                            // Apply current brightness via gamma
                            const brightness = Math.max(10, monitor.brightness ?? 100)
                            const gdiName = Overlay.getGdiDeviceName(b.left, b.top)
                            if (gdiName) {
                                Overlay.setGammaRamp(gdiName, brightness)
                            }
                            console.log(`Fullscreen detected on ${key}, switching to gamma ramp`)
                        }
                    }
                }
            } else {
                // No fullscreen — restore any gamma-active monitors to overlay
                for (const key in overlayGammaActive) {
                    if (overlayGammaActive[key]) {
                        const monitor = monitors[key]
                        if (monitor?.bounds) {
                            const gdiName = Overlay.getGdiDeviceName(monitor.bounds.left, monitor.bounds.top)
                            if (gdiName) Overlay.resetGammaRamp(gdiName)
                        }
                        // Restore overlay opacity
                        const brightness = monitor?.brightness ?? 100
                        const opacity = Math.round((1 - brightness / 100) * 230)
                        if (overlayHandles[key]) {
                            Overlay.setOverlayOpacity(overlayHandles[key], opacity)
                        }
                        delete overlayGammaActive[key]
                        console.log(`Fullscreen ended on ${key}, switching back to overlay`)
                    }
                }
            }
        } catch (e) {
            // Silently handle — fullscreen detection is best-effort
        }
    }, 2000)
}

function stopFullscreenDetection() {
    if (fullscreenCheckInterval) {
        clearInterval(fullscreenCheckInterval)
        fullscreenCheckInterval = null
    }
}
```

- [ ] **Step 6: Start fullscreen detection and clean up on app quit**

Find the `app.on('ready', ...)` or `app.whenReady()` handler. After the initial setup is complete (after `startMonitorThread()` is called), add:

```javascript
startFullscreenDetection()
```

Find the app quit handler (search for `app.on('will-quit'` or `app.on('before-quit'`). Add:

```javascript
stopFullscreenDetection()
destroyAllOverlayWindows()
```

Also search for `app.on('quit'` as a fallback. If there's a `mainWin.on('close'` handler, add cleanup there too.

- [ ] **Step 7: Handle display configuration changes**

Search for where display changes are detected. Electron has `screen.on('display-added')`, `screen.on('display-removed')`, `screen.on('display-metrics-changed')`. Find existing listeners for these events. If they exist, add `repositionAllOverlays()` in each handler. If they don't exist, add them after the app is ready:

```javascript
const { screen } = require('electron')
screen.on('display-added', () => { repositionAllOverlays() })
screen.on('display-removed', () => { syncOverlaysWithMonitors() })
screen.on('display-metrics-changed', () => { repositionAllOverlays() })
```

- [ ] **Step 8: Commit**

```bash
git add src/electron.js
git commit -m "feat: add overlay lifecycle management and fullscreen detection in electron.js"
```

---

### Task 4: Add `"overlay"` Type to `updateBrightness()` in `electron.js`

**Files:**
- Modify: `src/electron.js`

- [ ] **Step 1: Add overlay branch in `updateBrightness()`**

In `src/electron.js`, find `function updateBrightness()` at line 1997. The function has type-based branches for `ddcci`, `studio-display`, and `wmi`. Add an overlay branch.

Find the section starting at line 2059 (`} else if (monitor.type == "ddcci") {`). Before this line, add:

```javascript
    } else if (monitor.type === "overlay") {
      monitor.brightness = level
      monitor.brightnessRaw = level
      monitorsThread.send({
          type: "brightness",
          brightness: level,
          id: monitor.id
      })
```

- [ ] **Step 2: Include overlay in `updateAllBrightness()` filtering**

In `function updateAllBrightness()` at line 2150, find the line at 2157:
```javascript
if (monitor.type !== "none") {
```

This already covers overlay monitors since they're not `"none"`. No change needed here.

- [ ] **Step 3: Include overlay in profile brightness application**

Find the profile application code near line 1087:
```javascript
if (monitor.type == "wmi" || monitor.type == "studio-display" || (monitor.type == "ddcci" && monitor.brightnessType)) {
```

Change to:
```javascript
if (monitor.type == "wmi" || monitor.type == "studio-display" || monitor.type == "overlay" || (monitor.type == "ddcci" && monitor.brightnessType)) {
```

Search for ALL similar filtering patterns in `electron.js` that list supported types and add `monitor.type == "overlay"` to each. The key locations are:

1. Line ~1087 (profile brightness application)
2. Line ~1328 (overlay monitor count) — where it checks for ddcci/studio-display/wmi
3. Line ~3430 (menu profile setBrightness) — same pattern
4. Any other `monitor.type ==` chains that exclude `"none"`

For each, add `|| monitor.type == "overlay"`.

- [ ] **Step 4: Commit**

```bash
git add src/electron.js
git commit -m "feat: route overlay monitor brightness through main process"
```

---

### Task 5: Update BrightnessPanel UI to Show Overlay Monitors

**Files:**
- Modify: `src/components/BrightnessPanel.jsx`

- [ ] **Step 1: Include overlay type in monitor count**

In `src/components/BrightnessPanel.jsx`, find the `numMonitors` useMemo at line 26:

```javascript
if ((state.monitors[key].type != "none" || state.monitors[key].hdr === "active") && !(window.settings?.hideDisplays?.[key] === true)) localNumMonitors++;
```

Since overlay monitors have `type: "overlay"` (not `"none"`), they're already counted. No change needed here.

- [ ] **Step 2: Include overlay in linked levels valid monitor check**

Find the linked levels section at line 222:
```javascript
if(monitor.type == "wmi" || monitor.type == "studio-display" || (monitor.type == "ddcci" && monitor.brightnessType) || monitor.hdr === "active") {
```

Change to:
```javascript
if(monitor.type == "wmi" || monitor.type == "studio-display" || monitor.type == "overlay" || (monitor.type == "ddcci" && monitor.brightnessType) || monitor.hdr === "active") {
```

- [ ] **Step 3: Include overlay in individual monitor rendering**

Find the individual monitor rendering at line 265:
```javascript
if (monitor.type == "wmi" || monitor.type == "studio-display" || (monitor.type == "ddcci" && monitor.brightnessType) || monitor.hdr === "active") {
```

Change to:
```javascript
if (monitor.type == "wmi" || monitor.type == "studio-display" || monitor.type == "overlay" || (monitor.type == "ddcci" && monitor.brightnessType) || monitor.hdr === "active") {
```

- [ ] **Step 4: Add overlay icon in Slider name row**

In `src/components/Slider.jsx`, find the `getName` method at line 26. The icon currently shows a laptop icon for WMI or a monitor icon for others. Add an overlay icon:

Change line 30:
```javascript
<div className="icon" style={{display: (this.props.icon === false ? "none" : "block")}}>{(this.props.monitortype == "wmi" ? <span>&#xE770;</span> : <span>&#xE7F4;</span>)}</div>
```

To:
```javascript
<div className="icon" style={{display: (this.props.icon === false ? "none" : "block")}}>{(this.props.monitortype == "wmi" ? <span>&#xE770;</span> : <span>&#xE7F4;</span>)}{this.props.monitortype == "overlay" && <span className="overlay-badge" title="Overlay mode">&#xE81E;</span>}</div>
```

The icon `&#xE81E;` is the Segoe MDL2 "MapLayers" icon — a stacked-layers glyph that conveys "overlay." This renders as a small badge next to the monitor icon.

- [ ] **Step 5: Also update the extended layout icon in BrightnessPanel.jsx**

Find line 330 in `BrightnessPanel.jsx`:
```javascript
<div className="icon">{(monitor.type == "wmi" ? <span>&#xE770;</span> : <span>&#xE7F4;</span>)}</div>
```

Change to:
```javascript
<div className="icon">{(monitor.type == "wmi" ? <span>&#xE770;</span> : <span>&#xE7F4;</span>)}{monitor.type == "overlay" && <span className="overlay-badge" title="Overlay mode">&#xE81E;</span>}</div>
```

- [ ] **Step 6: Commit**

```bash
git add src/components/BrightnessPanel.jsx src/components/Slider.jsx
git commit -m "feat: show overlay monitors in brightness panel with overlay badge icon"
```

---

### Task 6: Update Settings Window for Overlay Monitors

**Files:**
- Modify: `src/components/SettingsWindow.jsx`
- Modify: `src/components/MonitorInfo.jsx`

- [ ] **Step 1: Include overlay monitors in all SettingsWindow type checks**

In `src/components/SettingsWindow.jsx`, find every `monitor.type == "none"` check and update to also exclude overlay monitors from being hidden. The key locations:

At line 443:
```javascript
if (monitor.type == "none") {
    return (<div key={monitor.name}></div>)
```
Change to:
```javascript
if (monitor.type == "none") {
    return (<div key={monitor.name}></div>)
```
(No change needed — overlay is not `"none"` so it will show.)

At line 550:
```javascript
if (monitor.type == "none") {
    return null
```
(Same — no change needed.)

At line 578:
```javascript
if (monitor.type == "none") {
    return (<div key={monitor.id}></div>)
```
(Same — no change needed.)

At line 680:
```javascript
if (monitor.type == "none") {
    return (<div key={monitor.id + ".brightness"}></div>)
```
(Same — no change needed.)

At line 1658 (getProfileMonitors):
```javascript
if (monitor.type == "none") {
    return (null)
```
(Same — no change needed.)

At line 1745:
```javascript
if(monitor.type === "none") return null;
```
(Same — no change needed.)

Since overlay monitors have `type: "overlay"` instead of `"none"`, they will automatically show up in all these sections. No changes needed to the type checks.

- [ ] **Step 2: Add "Overlay" to debug monitor type display in SettingsWindow**

Find `getDebugMonitorType` at line 979 in `SettingsWindow.jsx`. Add an overlay case:

After the `studio-display` case, add:
```javascript
} else if (type == "overlay") {
    return (<><b>Overlay</b> <span className="icon green vfix">&#xE73D;</span></>)
```

- [ ] **Step 3: Add brightness method dropdown in SettingsWindow**

Find the monitor details section (near line 823) that shows monitor info. After the HDR line (line 829), add a brightness method selector:

```javascript
<br />{T.t("SETTINGS_MONITORS_BRIGHTNESS_METHOD") || "Brightness Method"}: <select value={this.state.rawSettings?.overlayDisplays?.[monitor.key] === true ? "overlay" : this.state.rawSettings?.overlayDisplays?.[monitor.key] === false ? "hardware" : "auto"} onChange={(e) => {
    const overlayDisplays = Object.assign({}, this.state.rawSettings?.overlayDisplays)
    if (e.target.value === "overlay") {
        overlayDisplays[monitor.key] = true
    } else if (e.target.value === "hardware") {
        overlayDisplays[monitor.key] = false
    } else {
        delete overlayDisplays[monitor.key]
    }
    this.setSetting("overlayDisplays", overlayDisplays)
}}>
    <option value="auto">Auto</option>
    <option value="overlay">Overlay</option>
    <option value="hardware">Hardware Only</option>
</select>
```

- [ ] **Step 4: Update brightness display for overlay monitors**

In the same monitor details section, line 826:
```javascript
<br />{T.t("SETTINGS_MONITORS_DETAILS_BRIGHTNESS")}: <b>{(monitor.type == "none" ? T.t("GENERIC_NOT_SUPPORTED") : brightness)}</b>
```

This already works — overlay monitors will show their brightness value since they're not type `"none"`.

- [ ] **Step 5: Update MonitorInfo.jsx**

In `src/components/MonitorInfo.jsx`, find `getDebugMonitorType` at line 131. Add overlay case:

After the `studio-display` case (line 140), add:
```javascript
} else if (type == "overlay") {
    return (<><b>Overlay</b> <span className="icon green vfix">&#xE73D;</span></>)
```

Also update line 102 where brightness is shown:
```javascript
<br />{T.t("SETTINGS_MONITORS_DETAILS_BRIGHTNESS")}: <b>{(monitor.type == "none" ? T.t("GENERIC_NOT_SUPPORTED") : monitor.brightness)}</b>
```
(Already works — overlay is not `"none"`.)

- [ ] **Step 6: Commit**

```bash
git add src/components/SettingsWindow.jsx src/components/MonitorInfo.jsx
git commit -m "feat: add overlay type support and brightness method selector in settings"
```

---

### Task 7: Pass `overlayDisplays` Setting to Monitor Thread

**Files:**
- Modify: `src/electron.js`

- [ ] **Step 1: Include `overlayDisplays` in settings sent to monitor thread**

The settings object is already sent to the monitor thread wholesale at line 188-191:
```javascript
monitorsThreadReal.send({
    type: "settings",
    settings
})
```

Since `overlayDisplays` is stored in `settings`, it will be automatically included. No changes needed to the settings send.

However, when settings change at runtime (e.g., user changes brightness method), the monitor thread needs to be notified to re-classify monitors. Search for where settings updates trigger a monitor refresh. Find the settings change handler (search for `sendSettings` or IPC settings handler).

Find where `settings` are saved and `refreshMonitors` is triggered afterward. After settings that include `overlayDisplays` are saved, ensure a full refresh is triggered:

```javascript
// When overlayDisplays setting changes, trigger full monitor refresh
if (newSettings.overlayDisplays) {
    refreshMonitors(true)
}
```

Add this in the settings change handler, after settings are saved.

- [ ] **Step 2: Trigger overlay sync after settings change**

Search for where settings changes trigger `sendToAllWindows('monitors-updated', monitors)`. Add `syncOverlaysWithMonitors()` after it.

- [ ] **Step 3: Commit**

```bash
git add src/electron.js
git commit -m "feat: sync overlay state when settings change"
```

---

### Task 8: Handle Edge Cases and Final Integration

**Files:**
- Modify: `src/electron.js`
- Modify: `src/Monitors.js`

- [ ] **Step 1: Ensure overlay brightness persists across refreshes**

In `src/Monitors.js`, the `refreshMonitors` function (line 135) does partial refreshes for DDC/CI and WMI monitors. For overlay monitors, brightness is purely in-memory. Add a section after the WMI refresh (around line 205):

```javascript
            // Overlay monitors keep their brightness across refreshes (no hardware to query)
            // No action needed — brightness state is maintained in the monitors object
```

This is a comment-only addition for clarity. The monitors object retains `brightness` across partial refreshes since overlay monitors aren't re-queried.

- [ ] **Step 2: Handle overlay type in syncBrightness (BrightnessPanel.jsx)**

In `src/components/BrightnessPanel.jsx`, find `syncBrightness()` at line 140. The condition at line 147:
```javascript
if (monitors[idx].type != "none" && monitors[idx].brightness != lastLevels[idx]) {
```

This already works for overlay monitors. No change needed.

- [ ] **Step 3: Ensure overlay monitors have bounds data**

In `src/Monitors.js`, the `getMonitorsWin32()` function stores `bounds: monitor.sourceMode`. This is how we get the screen rectangle. Overlay monitors that come through Win32 detection will have bounds. Verify this is available by checking the `updateDisplay` merge logic at line 893 — it uses `Object.assign`, so bounds from Win32 will be preserved when DDC/CI detection later sets type to `"none"` (now `"overlay"`). This works correctly — no changes needed.

- [ ] **Step 4: Handle `connectorDevicePath` for gamma ramp**

For the gamma ramp fallback, we need a device name like `\\.\DISPLAY1`. The `win32-displayconfig` module provides this. Check what's available in the Win32 display data.

In `src/Monitors.js`, `getMonitorsWin32()` stores `connector: monitor.outputTechnology` but not the GDI device name. We need to add the GDI device name. However, looking at the `win32-displayconfig` module output, the `sourceConfigId` may contain the needed info.

For the gamma ramp, we need the GDI device name. Add it to the Win32 monitor data. In `getMonitorsWin32()` at line 605, after `sourceID`, add:

```javascript
gdiDeviceName: monitor.gdiDeviceName || monitor.sourceName || null,
```

If `win32-displayconfig` doesn't expose GDI device names directly, we can derive them from the display index. In `electron.js`, use Electron's `screen.getAllDisplays()` to map monitor bounds to GDI device names. Update the gamma ramp code in the fullscreen detection to use the Electron screen API:

In the `startFullscreenDetection` function, when switching to gamma:
```javascript
// Get GDI device name from Electron screen API
const { screen } = require('electron')
const electronDisplays = screen.getAllDisplays()
const electronDisplay = electronDisplays.find(d =>
    d.bounds.x === b.left && d.bounds.y === b.top
)
if (electronDisplay) {
    // On Windows, Electron doesn't expose GDI names directly.
    // Use the display index to construct the name: \\.\DISPLAY{n}
    const displayName = `\\\\.\\DISPLAY${electronDisplay.id}`
    Overlay.setGammaRamp(displayName, brightness)
}
```

Actually, `electronDisplay.id` is not the same as the GDI display index. The more reliable approach: enumerate displays using Win32 `EnumDisplayDevices` in the native module. But for simplicity, since gamma ramp is only a fallback for exclusive fullscreen, we can use the `screen` module's display label which on Windows maps to `\\.\DISPLAY1`, etc.

A simpler approach: add a native helper in `windows_overlay.cc` to enumerate GDI device names by monitor position.

Add to `windows_overlay.cc`:

```cpp
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

    // mi.szDevice is the GDI device name like L"\\\\.\\DISPLAY1"
    char deviceName[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, deviceName, sizeof(deviceName), NULL, NULL);
    return Napi::String::New(env, deviceName);
}
```

Add to the Init exports:
```cpp
exports.Set(Napi::String::New(env, "getGdiDeviceName"), Napi::Function::New(env, GetGdiDeviceName));
```

And export it in `index.js`:
```javascript
getGdiDeviceName: Overlay.getGdiDeviceName,
```

Then update the fullscreen detection gamma ramp code to use:
```javascript
const gdiName = Overlay.getGdiDeviceName(b.left, b.top)
if (gdiName) {
    Overlay.setGammaRamp(gdiName, brightness)
}
```

And update the gamma ramp reset code similarly:
```javascript
if (monitor?.bounds) {
    const gdiName = Overlay.getGdiDeviceName(monitor.bounds.left, monitor.bounds.top)
    if (gdiName) Overlay.resetGammaRamp(gdiName)
}
```

- [ ] **Step 5: Commit**

```bash
git add src/modules/tt-windows-utils/windows_overlay.cc src/modules/tt-windows-utils/index.js src/electron.js src/Monitors.js
git commit -m "feat: add GDI device name lookup and finalize overlay integration"
```

---

### Task 9: Add Overlay Badge CSS

**Files:**
- Modify: `src/scss/panel.scss` (or wherever panel styles live)

- [ ] **Step 1: Find and update the panel stylesheet**

Search for the panel stylesheet:
```bash
find src -name "*.scss" -o -name "*.css" | head -20
```

Add the overlay badge style:

```css
.overlay-badge {
    font-size: 0.65em;
    opacity: 0.6;
    margin-left: 2px;
    vertical-align: super;
}
```

- [ ] **Step 2: Commit**

```bash
git add src/scss/
git commit -m "feat: add overlay badge icon style"
```

---

### Task 10: Build and Manual Test

- [ ] **Step 1: Rebuild native modules**

```bash
cd src/modules/tt-windows-utils && npm run rebuild
```

Expected: Build succeeds without errors.

- [ ] **Step 2: Build the Electron app**

```bash
npm run build
```

Expected: Parcel bundles successfully.

- [ ] **Step 3: Run the app in dev mode**

```bash
npm start
```

Expected: App starts, system tray icon appears.

- [ ] **Step 4: Verify overlay monitors appear**

Check the brightness panel:
- Monitors that lack DDC/CI should appear with a brightness slider and an overlay badge icon
- Moving the slider should create a visible black overlay on that monitor
- At 100% brightness, no overlay visible
- At 0% brightness, monitor should be very dim but not fully black (~90% opacity)

- [ ] **Step 5: Verify settings**

Open Settings > Monitors:
- Overlay monitors should show "Overlay" as communication type
- "Brightness Method" dropdown should appear for all monitors
- Changing a DDC/CI monitor to "Overlay" mode should switch it to overlay dimming
- Changing back to "Auto" or "Hardware Only" should restore DDC/CI control

- [ ] **Step 6: Verify fullscreen fallback**

Open a fullscreen game or app on a monitor with overlay dimming:
- The overlay should remain visible over borderless fullscreen apps
- If exclusive fullscreen: within 2 seconds, dimming should switch to gamma ramp (visual: slightly different dimming quality but still dark)
- Exiting fullscreen should switch back to overlay

- [ ] **Step 7: Final commit**

```bash
git add -A
git commit -m "feat: complete overlay brightness support for unsupported monitors"
```
