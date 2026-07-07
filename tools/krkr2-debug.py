#!/usr/bin/env python3
"""Krkr2 Yuri debugger — PC-side control tool.
Talk to DebugLayer TCP server on Android device via ADB port forward.

Usage:
  adb forward tcp:9999 tcp:9999   (one-time setup)
  python3 krkr2-debug.py          (start interactive UI)

Keyboard shortcuts (immediate — no Enter key needed):
  Space   Step one frame
  s       Toggle slow-mo (0.5x / 1.0x)
  c       Capture textures for current frame
  r       Trigger RenderDoc capture (.rdc)
  n       Next draw call (in NAV mode)
  p       Previous draw call (in NAV mode)
  m       Toggle mode: NORMAL -> STEP -> SLOWMO -> NAV
  q       Quit
"""
import socket, struct, sys, time, subprocess, select

try:
    import termios, tty
    HAVE_TTY = True
except ImportError:
    HAVE_TTY = False

HOST = "127.0.0.1"
PORT = 9999

def send(cmd: int, payload: bytes = b"") -> bytes:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect((HOST, PORT))
    s.sendall(bytes([cmd]) + payload)
    resp = s.recv(4096)
    s.close()
    return resp

def step():
    send(0x01)

def set_mode(m: int):
    send(0x02, bytes([m]))

def set_rate(r: float):
    send(0x03, struct.pack("<f", r))

def capture():
    send(0x04)

def nav_next():
    send(0x05)

def nav_prev():
    send(0x06)

def nav_set(idx: int):
    send(0x07, struct.pack("<i", idx))

def trigger_rdoc():
    send(0x08)

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
    subprocess.run(
        ["adb", "pull", "/sdcard/Download/krkr2_debug", "./krkr2_debug"],
        capture_output=True
    )

MODE_NAMES = ["NORM", "STEP", "SLOW", "NAV"]

def interactive():
    print("\n=== Krkr2 Yuri Debugger ===")
    print("Keys: [Space]Step [s]lowmo [c]apture [r]doc [n/p]nav [m]ode [q]uit\n")

    last_refresh = 0
    status_line = ""

    if HAVE_TTY:
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        tty.setraw(fd)

    try:
        while True:
            now = time.time()

            # Refresh status display every 0.3s
            if now - last_refresh > 0.3:
                try:
                    fc, cc, ni, md = get_status()
                    mode_name = MODE_NAMES[md] if md < len(MODE_NAMES) else "?"
                    ni_str = f"{ni}/{cc}" if ni >= 0 else "final"
                    detail = ""
                    if ni >= 0:
                        info = get_draw_call_info(ni)
                        if info:
                            n, l, t, w, h = info
                            detail = f"  [{n}] ({l},{t},{w}×{h})"
                    status_line = f"  FRM {fc} | {mode_name} | call {ni_str}{detail}"
                    # Clear line and reprint
                    sys.stdout.write("\r\033[K" + status_line)
                    sys.stdout.flush()
                except Exception:
                    pass
                last_refresh = now

            # Check for keypress (non-blocking)
            if select.select([sys.stdin], [], [], 0.05)[0]:
                ch = sys.stdin.read(1)
                # Visual feedback on same line
                action = ""
                if ch == ' ':
                    step(); action = "Step"
                elif ch == 's':
                    try:
                        _, _, _, cur_md = get_status()
                        set_rate(0.5 if cur_md != 2 else 1.0)
                        action = "SlowMo " + ("0.5x" if cur_md != 2 else "1.0x")
                    except Exception:
                        action = "SlowMo err"
                elif ch == 'c':
                    capture(); action = "Capture"
                elif ch == 'r':
                    trigger_rdoc(); action = "RDC"
                elif ch == 'n':
                    nav_next(); action = "NavNext"
                elif ch == 'p':
                    nav_prev(); action = "NavPrev"
                elif ch == 'm':
                    try:
                        _, _, _, cur_md = get_status()
                        set_mode((cur_md + 1) % 4)
                        action = f"Mode {MODE_NAMES[(cur_md+1)%4]}"
                    except Exception:
                        action = "Mode err"
                elif ch == 'P':
                    pull_captures(); action = "Pulling..."
                elif ch == 'q':
                    break
                if action:
                    sys.stdout.write(f"\n\033[K  > {action}")
                    sys.stdout.flush()
                    last_refresh = 0  # force refresh
    finally:
        if HAVE_TTY:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
        print("\n  Quit")

if __name__ == "__main__":
    try:
        interactive()
    except ConnectionRefusedError:
        print("ERROR: Cannot connect. Did you run: adb forward tcp:9999 tcp:9999")
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n  Interrupted")
