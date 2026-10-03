# Works around a real bug in the pioarduino platform's build script
# (builder/main.py, BeforeUpload()): it only runs PlatformIO's own
# pattern-matching port autodetection when upload_port is completely
# unset --
#
#   if not env.subst("$UPLOAD_PORT"): env.AutodetectUploadPort()
#
# -- it never checks whether the *configured* value is itself a wildcard
# pattern (platformio.device.finder.is_pattern_port() would say yes for
# "/dev/cu.usb*"). So setting upload_port to a glob -- which PlatformIO's
# own docs describe as supported -- never actually gets resolved by this
# platform: it's passed straight through to esptool's --port argument as
# the literal string, producing exactly this real failure:
#   ERROR: A fatal error occurred: Could not open /dev/cu.usb*, the port
#   is busy or doesn't exist.
#
# So we resolve the glob ourselves, here, before that check ever runs.
# extra_scripts with the "pre:" prefix execute while this environment is
# still being set up, well before the upload action fires, so by the time
# BeforeUpload() looks at UPLOAD_PORT it's already a real device path and
# everything downstream (including that buggy check) works unmodified.
Import("env")

import glob

# macOS-specific -- see platformio.ini's upload_port comment for why this
# needs to be a pattern at all (the ESP32-S3's USB-Serial-JTOG peripheral
# doesn't get a stable /dev/cu.* name across replugs on this machine).
_PORT_GLOB = "/dev/cu.usb*"

_configured = env.subst("$UPLOAD_PORT")
if _configured and "*" in _configured:
    _matches = sorted(glob.glob(_PORT_GLOB))
    if len(_matches) == 1:
        print(f"[resolve_upload_port] {_PORT_GLOB} -> {_matches[0]}")
        env.Replace(UPLOAD_PORT=_matches[0])
    elif len(_matches) > 1:
        # Can happen for a few seconds right after a reset/replug, while
        # macOS hasn't cleaned up a stale device node yet -- see the
        # project chat history for why. Picking the first is a guess; if
        # it's wrong, unplug/replug once to settle on a single port, or
        # set upload_port to the exact path in platformio.ini.
        print(f"[resolve_upload_port] multiple matches for {_PORT_GLOB}: {_matches} "
              f"-- using {_matches[0]}")
        env.Replace(UPLOAD_PORT=_matches[0])
    else:
        print(f"[resolve_upload_port] no device matching {_PORT_GLOB} found "
              "(only matters for uploading -- ignore this during a plain build). "
              "Plug in the board -- holding BOOT while powering it on if it "
              "doesn't enter the bootloader on its own -- before uploading.")
