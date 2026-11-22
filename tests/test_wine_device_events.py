#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Device events of drivers hosted in the game (Wine patch 1041): a process with no plug and play service still tells
its own windows when a controller comes or goes, from sechost's notification thread and never from the driver's.

The behaviour itself is checked on a host Wine by tools/test_wine_hid_pad.py --hosted (a registered window is told
when the second controller comes and goes, and Windows.Gaming.Input lists it)."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/1041-sechost-deliver-hosted-drivers-device-events-in-the-process.patch"


def added(path):
    for diff in PATCH.read_text().split("diff --git ")[1:]:
        if diff.splitlines()[0].split(" b/", 1)[1] == path:
            return "\n".join(line[1:] for line in diff.splitlines() if line.startswith("+") and not line.startswith("+++"))
    raise AssertionError(path)


def new_side(path):
    """The patched file's lines that the diff shows, context and added, in order."""
    for diff in PATCH.read_text().split("diff --git ")[1:]:
        if diff.splitlines()[0].split(" b/", 1)[1] == path:
            return "\n".join(line[1:] for line in diff.splitlines()
                             if line[:1] in (" ", "+") and not line.startswith("+++"))
    raise AssertionError(path)


def in_order(text, *parts):
    at = 0
    for part in parts:
        at = text.index(part, at) + len(part)


def function(text, name):
    found = re.search(r"\n[^\n]*\b" + re.escape(name) + r"\(.*?\n\}\n", "\n" + text + "\n", re.S)
    assert found, name
    return found[0]


def main():
    # Hosted, every event goes to this process's sechost in place of the RPC to a service that is not there.
    in_order(new_side("dlls/ntoskrnl.exe/pnp.c"), "static void send_devicechange(", "if (hosted_queue_device_event)",
             "hosted_queue_device_event( path, code, data, size );\n        return;", "plugplay_send_event")
    # Found and turned on once, in wine_ntoskrnl_host_start before it starts the driver loop: not from a driver's thread,
    # which must not wait for the loader.
    in_order(new_side("dlls/ntoskrnl.exe/ntoskrnl.c"),
             'GetProcAddress( sechost, "__wine_queue_device_event" )', "hosted_queue_device_event( NULL, 0, NULL, 0 );",
             "GetSystemDirectoryW( path, MAX_PATH );")

    service = added("dlls/sechost/service.c")
    queue = function(service, "__wine_queue_device_event")
    # The driver's thread only queues: a window callback waits on the window's thread, which may be waiting on the
    # driver loop that sends the event.
    assert "callback" not in queue and "SendMessage" not in queue
    assert "list_add_tail( &local_device_events" in queue and "SetEvent( local_device_queued )" in queue
    # Only once turned on, and only for programs already registered: later ones were not there when it happened.
    assert "local_device_events_on && " in queue and "!list_empty( &device_notify_list )" in queue
    assert "event->serial = device_notify_serial;" in queue
    assert "notify->serial = ++device_notify_serial;" in service
    dispatch = function(service, "dispatch_device_event")
    assert "if (notify->serial > serial) continue;" in dispatch
    # The queued events pass the same filters and callbacks as the service's, which reach every registration.
    loop = function(service, "local_device_notify_loop")
    assert "dispatch_device_event( event->code, event->path, (DEV_BROADCAST_HDR *)event->data, event->serial );" in loop
    assert "dispatch_device_event( code, path, (DEV_BROADCAST_HDR *)buf, ~(UINT64)0 );" in service
    # The notification thread waits for this process's events once they are on, before trying the service, and a
    # thread started earlier turns to them when the service is not there. Elsewhere a failed service ends it as before.
    proc = new_side("dlls/sechost/service.c").split("static DWORD WINAPI device_notify_proc(", 1)[1]
    in_order(proc, "if (local) return local_device_notify_loop();", "RpcStringBindingComposeW",
             "if (local) return local_device_notify_loop();", 'ERR("failed to open RPC handle', "return 1;")
    assert "@ stdcall __wine_queue_device_event(ptr long ptr long)" in added("dlls/sechost/sechost.spec")
    # The kit's PE modules carry both halves.
    modules = " ".join(re.findall(r'^PE_MODULES="([^"\n]+)"', (ROOT / "tools/build_wine_ps5.sh").read_text(), re.M))
    assert {"sechost", "ntoskrnl.exe"} <= set(modules.replace("$PE_MODULES", "").split())
    # The lab test checks the behaviour hosted too, with Windows.Gaming.Input no longer skipped there.
    lab = (ROOT / "tests/lab/pw_hid_pad.c").read_text()
    assert "RegisterDeviceNotificationW(listener" in lab
    assert 'GetEnvironmentVariableA("WINE_NTOSKRNL_HOSTED"' not in lab
    print("Wine device events passed: hosted drivers' events are queued and delivered in the process")


if __name__ == "__main__":
    main()
