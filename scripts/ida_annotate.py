import hashlib
from pathlib import Path

import ida_bytes
import ida_name
import ida_nalt

EXPECTED_BASE = 0x140000000
EXPECTED_SHA256 = "ebfd5d43a165dcec5d4007b9d41ed5b2ed122fcd02ecfef7e4ed35b40826821f"
FUNCTIONS = (
    (0x140010850, "Azimuth_ReadTitleResolutionHeight",
     "Verified: returns the title's stored height when present; otherwise derives the default, or returns the unsupported sentinel."),
    (0x1400108D0, "Azimuth_NormalizeDisplayHeight",
     "Verified arithmetic: validates supplied dimensions/aspect ratio and returns an even candidate height from 360 to 4320, or zero."),
    (0x140010990, "Azimuth_BuildResolutionHeightOptions",
     "Verified: builds sorted heights from 720, 1080, 1440, 2160, 2880, 4320, then inserts the derived height and twice that height when allowed."),
    (0x140010CA0, "Azimuth_BuildResolutionChoiceList",
     "Verified: uses the height-options builder when the title supports that setting; otherwise returns a one-element list containing 1080."),
    (0x140010FA0, "Azimuth_SelectSupportedResolution",
     "Verified: scans the sorted list and selects the greatest entry not exceeding the stored value, or the first entry if all exceed it."),
    (0x140023590, "Azimuth_VectorUInt32_Insert",
     "Verified in decompilation: inserts a uint32 into a three-pointer vector and reallocates/copies through Azimuth's allocator helper when needed."),
    (0x140106DF0, "Azimuth_BuildLauncherDisplayRows",
     "Verified: creates launcher display rows; when supported, passes the resolution choices and selected index to the resolution-row builder."),
    (0x14011F290, "Azimuth_BuildResolutionLauncherRow",
     "Verified: builds the launcher row labeled Resolution from the supplied sorted choices and selected index."),
)


def main():
    base = ida_nalt.get_imagebase()
    if base != EXPECTED_BASE:
        raise RuntimeError(f"Unexpected image base: {base:#x}")
    input_path = ida_nalt.get_input_file_path()
    if not input_path or hashlib.sha256(Path(input_path).read_bytes()).hexdigest().lower() != EXPECTED_SHA256:
        raise RuntimeError("Input executable SHA-256 does not match the analyzed Azimuth 1.0.3 image")

    target_prologue = bytes.fromhex("48 89 5c 24 20 56 57 41 56 48 83 ec 60")
    helper_prologue = bytes.fromhex("48 89 6c 24 20 57 41 54 41 56 48 83 ec 20")
    if ida_bytes.get_bytes(0x140010990, len(target_prologue)) != target_prologue:
        raise RuntimeError("Resolution-options function bytes do not match the analyzed Azimuth 1.0.3 IDB")
    if ida_bytes.get_bytes(0x140023590, len(helper_prologue)) != helper_prologue:
        raise RuntimeError("Vector-insert helper bytes do not match the analyzed Azimuth 1.0.3 IDB")

    for address, name, comment in FUNCTIONS:
        if not ida_name.set_name(address, name, ida_name.SN_FORCE):
            raise RuntimeError(f"Could not name {address:#x}")
        ida_bytes.set_cmt(address, comment, True)
        print(f"named {address:#x} {name}")


if __name__ == "__main__":
    main()
