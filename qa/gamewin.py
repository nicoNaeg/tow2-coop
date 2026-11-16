"""Window input for test copies: post keys to a process's main window through its own message queue (no focus
change, no OS-level input), and take a picture with qa/winshot.ps1.

    from gamewin import Game; g = Game(pid); g.key("SPACE"); g.key("SPACE", hold_s=3); g.shot("x.png")"""
import ctypes
import subprocess
import time
from ctypes import wintypes
from pathlib import Path

user32 = ctypes.WinDLL("user32", use_last_error=True)
user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
user32.IsWindowVisible.argtypes = [wintypes.HWND]
user32.MapVirtualKeyW.argtypes = [wintypes.UINT, wintypes.UINT]
EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
user32.EnumWindows.argtypes = [EnumProc, wintypes.LPARAM]
user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]

VK = {"SPACE": 0x20, "ENTER": 0x0D, "ESCAPE": 0x1B, "TAB": 0x09, "UP": 0x26, "DOWN": 0x28, "LEFT": 0x25, "RIGHT": 0x27,
      "SHIFT": 0x10, "CTRL": 0x11, "F5": 0x74, "F9": 0x79}
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
HERE = Path(__file__).resolve().parent


def vk_of(key):
    key = key.upper()
    if key in VK:
        return VK[key]
    if len(key) == 1 and key.isalnum():
        return ord(key)
    raise ValueError(f"unknown key {key}")


class Game:
    def __init__(self, pid):
        self.pid = pid

    def hwnd(self):
        found = []

        def cb(h, _):
            p = wintypes.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(p))
            if p.value == self.pid and user32.IsWindowVisible(h) and user32.GetWindowTextLengthW(h) > 0:
                found.append(h)
            return True

        user32.EnumWindows(EnumProc(cb), 0)
        if not found:
            raise RuntimeError(f"pid {self.pid} has no visible window")
        return found[0]

    def key(self, key, hold_s=0.08):
        """Key press; with hold_s > 0.5 auto-repeat messages are sent while held, like a real keyboard."""
        h, vk = self.hwnd(), vk_of(key)
        scan = user32.MapVirtualKeyW(vk, 0)
        user32.PostMessageW(h, WM_KEYDOWN, vk, 1 | (scan << 16))
        t_end = time.time() + hold_s
        while time.time() < t_end:
            time.sleep(min(0.05, max(0.0, t_end - time.time())))
            if hold_s > 0.5:
                user32.PostMessageW(h, WM_KEYDOWN, vk, 1 | (scan << 16) | (1 << 30))  # repeat: previous state down
        user32.PostMessageW(h, WM_KEYUP, vk, 1 | (scan << 16) | (1 << 30) | (1 << 31))

    def click(self, button="left", hold_s=0.08):
        """Mouse button press at the window centre (gameplay: the viewport takes it; menus need widget calls)."""
        h = self.hwnd()
        down, up, mk = {"left": (0x0201, 0x0202, 1), "right": (0x0204, 0x0205, 2)}[button]
        l = (200 << 16) | 300
        user32.PostMessageW(h, down, mk, l)
        time.sleep(hold_s)
        user32.PostMessageW(h, up, 0, l)

    def shot(self, out):
        subprocess.run(["powershell", "-NoProfile", "-File", str(HERE / "winshot.ps1"), "-ProcessId", str(self.pid), "-Out", str(out)],
                       check=True, capture_output=True)
        return out
