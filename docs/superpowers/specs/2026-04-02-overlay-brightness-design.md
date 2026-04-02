# Overlay Brightness for Unsupported Monitors

**Date:** 2026-04-02
**Repo:** https://github.com/Mihai-Tudosa/twinkle-tray-for-non-supported (fork of xanderfrangos/twinkle-tray)

## Summary

Extend Twinkle Tray so that monitors without DDC/CI or WMI brightness support get a software-based dimming solution: a native Win32 overlay window (black, variable opacity) positioned over the display. Users can also opt any supported monitor into overlay mode via settings.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Overlay availability | Per-monitor user choice, defaults to overlay for unsupported | Maximum flexibility |
| Input handling | Click-through | Overlay is purely visual; all input passes to windows below |
| Fullscreen behavior | Always on top; gamma ramp fallback for exclusive fullscreen | Win32 TOPMOST works for borderless; gamma ramp covers exclusive fullscreen at GPU level |
| Minimum brightness | Cap at ~90% opacity (never fully opaque) | Prevents "losing" a monitor at 0% brightness |
| App exit behavior | Overlays disappear | Win32 windows die with the process; natural Electron behavior |
| Overlay rendering | Native Win32 via tt-windows-utils, not Electron BrowserWindow | Avoids compositing overhead; DWM handles layered windows at near-zero cost |

## 1. Native Overlay (C++ in `tt-windows-utils`)

Three new exported functions added to the existing `tt-windows-utils` native module:

- **`createOverlay(x, y, width, height)`** — Returns a handle/ID. Creates a Win32 window with styles `WS_POPUP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW`. Black background, click-through, no taskbar entry. Positioned to cover the target monitor's bounds.
- **`setOverlayOpacity(handle, opacity)`** — Sets the window's alpha via `SetLayeredWindowAttributes`. Opacity 0 = fully transparent (100% brightness), opacity ~230 = near-opaque (~90% coverage, the cap at 0% brightness).
- **`destroyOverlay(handle)`** — Destroys the window.

Two additional functions for exclusive fullscreen fallback:

- **`setGammaRamp(deviceName, brightness)`** — Applies a dimmed gamma ramp to the specified display adapter using `SetDeviceGammaRamp`. Brightness 0-100 maps to a proportionally reduced linear ramp (capped so 0% brightness still shows ~10% of original output).
- **`resetGammaRamp(deviceName)`** — Restores the default linear gamma ramp.

## 2. Monitor Type & Brightness Flow

### New monitor type

A new type `"overlay"` is added alongside `"ddcci"`, `"wmi"`, `"studio-display"`, and `"none"`.

### Detection changes in `Monitors.js`

During `getAllMonitors()`: monitors that would be assigned `type: "none"` are automatically assigned `type: "overlay"` instead. They are still detected and enumerated — they use the overlay method for brightness.

Users can also force any monitor to `type: "overlay"` via settings. This override is stored per-monitor alongside existing per-monitor settings (remaps, custom VCP codes, etc.).

### Brightness setting

`setBrightness()` in `Monitors.js` gets a new branch:

```javascript
if (monitor.type === "overlay") {
    if (monitor.exclusiveFullscreen) {
        // Exclusive fullscreen detected — use gamma ramp
        const brightness = Math.max(10, value) // cap at 10% minimum
        ttWindowsUtils.setGammaRamp(monitor.deviceName, brightness)
    } else {
        // Normal mode — use overlay window
        // 100 brightness → 0 opacity (transparent)
        // 0 brightness → ~230 opacity (90% black)
        const opacity = Math.round((1 - value / 100) * 230)
        ttWindowsUtils.setOverlayOpacity(monitor.overlayHandle, opacity)
    }
}
```

### Exclusive fullscreen detection

The app already has the `node-active-window` module. On a polling interval (or via a window event hook in `tt-windows-utils`):

1. Poll every 2 seconds (or hook `EVENT_SYSTEM_FOREGROUND` via `SetWinEventHook` for instant detection) whether the foreground window is in exclusive fullscreen mode.
2. Identify which monitor it occupies.
3. For overlay monitors on that display: hide the overlay window, activate gamma ramp dimming.
4. When the exclusive fullscreen app exits: reset gamma ramp, restore overlay window.

## 3. UI Changes

### BrightnessPanel / Slider

Monitors with `type: "overlay"` render the same slider as DDC/CI monitors. A small icon next to the monitor name indicates overlay mode (e.g., a layered-squares icon or a screen-with-shade icon).

### Settings Window (MonitorInfo)

- Instead of "Not supported" for overlay monitors, show "Overlay mode".
- Per-monitor dropdown: **"Brightness method"**
  - **Auto** — Uses DDC/CI/WMI if available, overlay if not (default).
  - **Overlay** — Forces overlay mode regardless of hardware support.
  - **Hardware only** — Uses DDC/CI/WMI only; disabled/greyed out if the monitor doesn't support it.

### Hotkeys, Linked Levels, Remaps

No changes needed. These all funnel through `setBrightness()`, which now has the overlay branch.

## 4. Overlay Lifecycle & Edge Cases

### App startup
After monitor detection, create overlay windows for all `type: "overlay"` monitors. Restore their last-known brightness (opacity) from saved settings.

### App exit
All overlay windows are destroyed automatically (Win32 windows die with the process). Full brightness restored.

### Monitor hotplug
The app already re-runs `getAllMonitors()` on display change events. New unsupported monitors get overlays created. Removed monitors get overlays destroyed.

### Resolution / position change
Re-query monitor bounds via `win32-displayconfig` and reposition the overlay window via `SetWindowPos`.

### Sleep / wake
Overlays persist across sleep — Win32 windows survive sleep/wake. Opacity state is maintained.

### Multi-monitor
Each overlay is an independent window positioned over its specific monitor. No interference between them.

### Exclusive fullscreen
`WS_EX_TOPMOST` keeps the overlay above borderless fullscreen apps. For true exclusive fullscreen (bypasses DWM), the app detects this and falls back to `SetDeviceGammaRamp` for that monitor, switching back to the overlay when the exclusive fullscreen app exits.

## 5. Files to Modify

| File | Changes |
|------|---------|
| `src/modules/tt-windows-utils/` | Add overlay window management (create/destroy/opacity) and gamma ramp functions |
| `src/Monitors.js` | Add `"overlay"` type, overlay creation/destruction lifecycle, exclusive fullscreen detection, `setBrightness` overlay branch |
| `src/electron.js` | Initialize overlays after monitor detection, handle overlay cleanup, pass overlay settings |
| `src/components/BrightnessPanel.jsx` | Show overlay icon indicator next to overlay monitors |
| `src/components/Slider.jsx` | No changes needed (already generic) |
| `src/components/SettingsWindow.jsx` | Add "Brightness method" dropdown per monitor, show "Overlay mode" instead of "Not supported" |
| `src/components/MonitorInfo.jsx` | Display overlay status instead of "Not supported" |
| `package.json` | No changes expected (tt-windows-utils already in build pipeline) |
