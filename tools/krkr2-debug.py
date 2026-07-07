#!/usr/bin/env python3
"""Krkr2 Yuri debugger — PC-side control tool.
Talk to DebugLayer TCP server on Android device via ADB port forward.

Usage:
  adb forward tcp:9999 tcp:9999   (one-time setup)
  python3 krkr2-debug.py          (start interactive UI)

Keyboard shortcuts within UI:
  Space   Step one frame
  s       Toggle slow-mo (0.5x / 1.0x)
  c       Capture textures for current frame
  r       Trigger RenderDoc capture (.rdc)
  n       Next draw call (in NAV mode)
  p       Previous draw call (in NAV mode)
  m       Toggle mode: NORMAL -> STEP -> SLOWMO
  q       Quit
"""
import socket, struct, sys, time, os, subprocess, threading

HOST = "127.0.0.1"
PORT = 9999

def send(cmd: int, payload: bytes = b"") -> bytes:
    """Send a command and return response."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, PORT))
    s.sendall(bytes([cmd]) + payload)
    resp = s.recv(4096)
    s.close()
    return resp

def step():
    send(0x01)
    print("  > Step")

def set_mode(m: int):
    send(0x02, bytes([m]))
    print(f"  > Mode {m}")

def set_rate(r: float):
    send(0x03, struct.pack("<f", r))
    print(f"  > SlowMo {r:.2f}x")

def capture():
    send(0x04)
    print("  > Capture triggered")

def nav_next():
    send(0x05)
    print("  > Nav next")

def nav_prev():
    send(0x06)
    print("  > Nav prev")

def nav_set(idx: int):
    send(0x07, struct.pack("<i", idx))
    print(f"  > Nav to {idx}")

def trigger_rdoc():
    send(0x08)
    print("  > RenderDoc capture triggered")

def get_status():
    resp = send(0x09)
    if len(resp) >= 32:
        fc = struct.unpack("<i", resp[0:4])[0]
        cc = struct.unpack("<i", resp[4:8])[0]
        ni = struct.unpack("<i", resp[8:12])[0]
        md = resp[12]
        return fc, cc, ni, md
    return 0, 0, -1, 0

def get_draw_call_info(idx: int):
    resp = send(0x0A, struct.pack("<i", idx))
    if len(resp) >= 20:
        name_len = struct.unpack("<i", resp[0:4])[0]
        name = resp[4:4+name_len].decode('ascii', errors='replace')
        left = struct.unpack("<i", resp[4+name_len:8+name_len])[0]
        top = struct.unpack("<i", resp[8+name_len:12+name_len])[0]
        w = struct.unpack("<i", resp[12+name_len:16+name_len])[0]
        h = struct.unpack("<i", resp[16+name_len:20+name_len])[0]
        return name, left, top, w, h
    return None

def pull_captures():
    """Pull captured textures from device."""
    print("  > Pulling captures...")
    subprocess.run(
        ["adb", "pull", "/sdcard/Download/krkr2_debug", "./krkr2_debug"],
        capture_output=True
    )
    print("  > Done")

def interactive():
    """Simple input-based interactive loop."""
    mode_names = ["NORM", "STEP", "SLOW", "NAV"]
    print("\n=== Krkr2 Yuri Debugger ===")
    print("Connected to ADB port 9999")
    print("Keys: [Space]Step [s]lowmo [c]apture [r]doc [n]ext [p]rev [m]ode [q]uit\n")

    last_refresh = 0
    while True:
        now = time.time()
        if now - last_refresh > 0.5:
            fc, cc, ni, md = get_status()
            mode_name = mode_names[md] if md < len(mode_names) else "?"
            ni_str = f"{ni}/{cc}" if ni >= 0 else "final"
            detail = ""
            if ni >= 0:
                info = get_draw_call_info(ni)
                if info:
                    name, l, t, w, h = info
                    detail = f"  [{name}] rect=({l},{t},{w},{h})"
            print(f"\r  FRM {fc} | {mode_name} | call {ni_str} | {detail}", end=" " * 10)
            last_refresh = now

        import select
        if select.select([sys.stdin], [], [], 0.1)[0]:
            ch = sys.stdin.read(1)
            if ch == ' ':
                step()
            elif ch == 's':
                set_rate(0.5 if get_status()[3] != 2 else 1.0)
            elif ch == 'c':
                capture()
            elif ch == 'r':
                trigger_rdoc()
            elif ch == 'n':
                nav_next()
            elif ch == 'p':
                nav_prev()
            elif ch == 'm':
                set_mode((get_status()[3] + 1) % 4)
            elif ch == 'P':
                pull_captures()
            elif ch == 'q':
                print("\n  Quit")
                break
            last_refresh = 0  # refresh immediately after action

if __name__ == "__main__":
    try:
        interactive()
    except KeyboardInterrupt:
        print("\n  Interrupted")
    except ConnectionRefusedError:
        print("ERROR: Cannot connect. Did you run: adb forward tcp:9999 tcp:9999")
        sys.exit(1)
