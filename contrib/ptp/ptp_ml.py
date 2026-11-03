#!/usr/bin/env python3
"""Talk to Magic Lantern over USB (PTP), using ML's CHDK-style operation
0x9999 (src/ptp-chdk.c): read/write camera memory without the card.

  ptp_ml.py info                       USB device and CHDK protocol version
  ptp_ml.py read ADDR LEN [FILE]       read memory (hex dump, or save)
  ptp_ml.py words ADDR [COUNT]         read 32-bit words (registers)
  ptp_ml.py write ADDR VALUE           write one 32-bit word
  ptp_ml.py mailbox                    list ML's PTP mailbox slots
  ptp_ml.py log [FILE]                 fetch a text slot (--slot NAME, default "saf")
  ptp_ml.py crlog [FILE]               crop_rec's DebugMsg trace (AF / Freeze trace),
                                       formatted here like ML/FRZLOG.TXT; works while
                                       LiveView is frozen. --last N: newest N entries
  ptp_ml.py crcmd start|pause|resume|savesaf
                                       crop_rec: start the Freeze trace, pause / resume
                                       capture, write ML/SAF.TXT
  ptp_ml.py saf                        crop_rec's Hybrid AF (SAF) info, as ML/SAF.TXT
  ptp_ml.py aflog start|stop|clear|save  crop_rec's AF log (one record per phase-detect
                                       result); save: ML/AFLOGnn.CSV on the card
  ptp_ml.py aflog [FILE.csv]           dump it as CSV (--last N); --follow: print new
                                       records live (Ctrl-C to stop)
  ptp_ml.py lvs [FILE]                 Canon's LV settings block (0xC4868, 0xBF8 bytes),
                                       as crop_rec's "Dump LV settings" (LVS1 header)
  ptp_ml.py lvci                       crop_rec: Canon's LV capture-info blocks, standby vs
                                       H.264 recording (word-by-word differences)
  ptp_ml.py lv [SECONDS]               remote LiveView test: on, wait, off
  ptp_ml.py get CAMFILE [LOCAL]        download a file from the card
                                       (e.g. B:/DCIM/100CANON/M19-1234.MLV)
  ptp_ml.py findmlv                    find MLV file names in camera RAM
  ptp_ml.py ls DIR                     list a card directory (e.g. B:/DCIM/)
  ptp_ml.py latest [LOCAL]             download the newest MLV in DCIM
  ptp_ml.py console                    ML's console text (printf output)
  ptp_ml.py put LOCAL CAMFILE          upload a file to the card (read back to verify)
  ptp_ml.py rm CAMFILE / mkdir DIR / mv SRC DST   card file operations (ML 2026-10-19+)
  ptp_ml.py install [BUILD.zip]        install an ML build zip on the card: changed
                                       files only, each verified, autoexec.bin last;
                                       restart the camera afterwards
  --lv                                 run the command with remote LiveView on
                                       (the camera leaves LV when tethered)

EOS M2 version of rea-work scripts/ptp_ml.py (M6II). The mailbox address
comes from magiclantern.sym of the build on the card (--sym, default: this
tree's platform/EOSM2.103 build).  Needs pyusb and access to the USB
device (run with sudo, or add a udev rule for Canon, vendor 04a9).
"""
import argparse
import os
import struct
import sys

import usb.core
import usb.util

CANON_VID = 0x04A9
PTP_OC_OpenSession = 0x1002
PTP_OC_CloseSession = 0x1003
PTP_OC_CHDK = 0x9999
PTP_CHDK_Version, PTP_CHDK_GetMemory, PTP_CHDK_SetMemory, PTP_CHDK_CallFunction = 0, 1, 2, 3
PTP_CHDK_TempData, PTP_CHDK_DownloadFile = 4, 6
PTP_CHDK_ML_ListDir = 0x4D4C   # ML extension (src/ptp-chdk.c)
PTP_CHDK_ML_FileOp = 0x4D4D    # ML extension: 1 mkdir, 2 rename, 3 remove
PTP_CHDK_ML_ReadRange = 0x4D4E # ML extension: 64-bit offset read / file size
RANGE_CHUNK = 16 << 20
PTP_CHDK_UploadFile = 5
HERE = os.path.dirname(os.path.abspath(__file__))
PLATFORM = os.path.normpath(os.path.join(HERE, "..", "..", "platform", "EOSM2.103"))
DEFAULT_BUILD = None    # "make zip" output; pass it to install
PTP_RC_OK = 0x2001
# Canon EOS remote control (as EOS Utility / gphoto2 use it)
PTP_OC_EOS_SetDevicePropValueEx = 0x9110
PTP_OC_EOS_SetRemoteMode = 0x9114
PTP_OC_EOS_SetEventMode = 0x9115
PTP_OC_EOS_GetEvent = 0x9116
PTP_DPC_EOS_EVFOutputDevice = 0xD1B0   # 1 camera screen, 2 PC (bitmask)
PTP_DPC_EOS_EVFMode = 0xD1B1
CONTAINER_CMD, CONTAINER_DATA, CONTAINER_RESP = 1, 2, 3
MAILBOX_MAGIC = 0x4D4C4D42
MAILBOX_SLOTS = 8
CHUNK = 1 << 20     # the camera stages each read in an ML buffer

DEFAULT_SYM = os.path.join(PLATFORM, "magiclantern.sym")
CR_PTP_MAGIC = 0x31505243   # "CRP1", crop_rec.c struct cr_ptp
LVS_ADDR, LVS_SIZE = 0xC4868, 0xBF8


class PTP:
    def __init__(self):
        dev = None
        for d in usb.core.find(find_all=True, idVendor=CANON_VID):
            for cfg in d:
                for intf in cfg:
                    if intf.bInterfaceClass == 6:   # still image (PTP)
                        dev, self.intf = d, intf
                        break
        if dev is None:
            sys.exit("no Canon PTP device found (camera on, USB connected?)")
        self.dev = dev
        if dev.is_kernel_driver_active(self.intf.bInterfaceNumber):
            dev.detach_kernel_driver(self.intf.bInterfaceNumber)
        usb.util.claim_interface(dev, self.intf.bInterfaceNumber)
        eps = self.intf.endpoints()
        self.ep_out = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT
                           and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
        self.ep_in = next(e for e in eps if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
                          and usb.util.endpoint_type(e.bmAttributes) == usb.util.ENDPOINT_TYPE_BULK)
        self.tid = 0
        self.transact(PTP_OC_OpenSession, [1])

    def _cmd(self, code, params):
        self.tid += 1
        hdr = struct.pack("<IHHI", 12 + 4 * len(params), CONTAINER_CMD, code, self.tid)
        self.ep_out.write(hdr + b"".join(struct.pack("<I", p & 0xFFFFFFFF) for p in params))

    def _read_container(self):
        data = bytes(self.ep_in.read(max(512, self.ep_in.wMaxPacketSize), timeout=10000))
        length, ctype, code, tid = struct.unpack_from("<IHHI", data)
        while len(data) < length:
            data += bytes(self.ep_in.read(min(length - len(data) + 512, 1 << 20), timeout=10000))
        return ctype, code, data[12:length]

    def transact(self, code, params, data_out=None, expect_data=False):
        self._cmd(code, params)
        if data_out is not None:
            self.ep_out.write(struct.pack("<IHHI", 12 + len(data_out), CONTAINER_DATA, code, self.tid) + data_out)
        payload = b""
        ctype, rcode, body = self._read_container()
        if ctype == CONTAINER_DATA:
            payload = body
            ctype, rcode, body = self._read_container()
        if ctype != CONTAINER_RESP or rcode != PTP_RC_OK:
            raise RuntimeError(f"PTP op {code:#x} failed: type {ctype} code {rcode:#x}")
        rparams = struct.unpack("<%dI" % (len(body) // 4), body[:len(body) // 4 * 4])
        return payload, rparams

    def chdk_version(self):
        return self.transact(PTP_OC_CHDK, [PTP_CHDK_Version])[1]

    def read(self, addr, length):
        if addr == 0:
            # the camera answers address 0 with an error and no data phase,
            # which leaves the session stuck
            raise ValueError("GetMemory can't read address 0")
        out = b""
        while len(out) < length:
            n = min(CHUNK, length - len(out))
            out += self.transact(PTP_OC_CHDK, [PTP_CHDK_GetMemory, addr + len(out), n], expect_data=True)[0]
        return out

    def words(self, addr, count=1):
        return struct.unpack("<%dI" % count, self.read(addr, 4 * count))

    def write32(self, addr, value):
        self.transact(PTP_OC_CHDK, [PTP_CHDK_SetMemory, addr, 4], data_out=struct.pack("<I", value))

    def download(self, camfile, progress=True):
        self.transact(PTP_OC_CHDK, [PTP_CHDK_TempData, 0], data_out=camfile.encode())
        self._cmd(PTP_OC_CHDK, [PTP_CHDK_DownloadFile])
        first = bytes(self.ep_in.read(1 << 16, timeout=20000))
        length, ctype, rcode, _ = struct.unpack_from("<IHHI", first)
        if ctype != CONTAINER_DATA:
            raise RuntimeError(f"download of {camfile} failed: code {rcode:#x}")
        chunks = [first[12:]]
        got = len(first)
        while got < length:
            c = bytes(self.ep_in.read(min(length - got + 512, 1 << 20), timeout=20000))
            chunks.append(c)
            got += len(c)
            if progress:
                if (got >> 20) != ((got - len(c)) >> 20):
                    print(f"\r{got >> 20} / {length >> 20} MB", end="", file=sys.stderr, flush=True)
        if progress:
            print(file=sys.stderr)
        ctype, rcode, _ = self._read_container()
        if ctype != CONTAINER_RESP or rcode != PTP_RC_OK:
            raise RuntimeError(f"download of {camfile} failed: code {rcode:#x}")
        return b"".join(chunks)[:length - 12]

    def file_size64(self, camfile):
        """64-bit file size via ReadRange (None if the camera's ML lacks it)"""
        try:
            self.transact(PTP_OC_CHDK, [PTP_CHDK_TempData, 0], data_out=camfile.encode())
            lo, hi = self.transact(PTP_OC_CHDK, [PTP_CHDK_ML_ReadRange, 0, 0, 0])[1][:2]
        except RuntimeError:
            return None
        return lo | hi << 32

    def read_range(self, camfile, offset, length):
        self.transact(PTP_OC_CHDK, [PTP_CHDK_TempData, 0], data_out=camfile.encode())
        return self.transact(PTP_OC_CHDK, [PTP_CHDK_ML_ReadRange, offset & 0xFFFFFFFF, offset >> 32, length],
                             expect_data=True)[0]

    def download_ranged(self, camfile, out, size, progress=True):
        """stream a file of any size to the open file `out`"""
        done = 0
        chunk = RANGE_CHUNK
        while done < size:
            try:
                data = self.read_range(camfile, done, min(chunk, size - done))
            except RuntimeError:
                # the camera stages each read in one buffer; on the M2 large ones may not fit
                if chunk <= 64 << 10:
                    raise
                chunk //= 2
                continue
            if not data:
                raise RuntimeError(f"read of {camfile} at {done} returned nothing")
            out.write(data)
            done += len(data)
            if progress:
                print(f"\r{done >> 20} / {size >> 20} MB", end="", file=sys.stderr, flush=True)
        if progress:
            print(file=sys.stderr)
        return done

    def listdir(self, path):
        """[(is_dir, size, timestamp, name)]"""
        self.transact(PTP_OC_CHDK, [PTP_CHDK_TempData, 0], data_out=path.encode())
        text = self.transact(PTP_OC_CHDK, [PTP_CHDK_ML_ListDir], expect_data=True)[0]
        out = []
        for line in text.decode(errors="replace").splitlines():
            parts = line.split(" ", 3)
            if len(parts) == 4:
                try:
                    size, ts = int(parts[1]) & 0xFFFFFFFF, int(parts[2]) & 0xFFFFFFFF
                except ValueError:      # builds before f73cc286c's fix
                    size, ts = 0, 0
                out.append((parts[0].startswith("D"), size, ts, parts[3]))
        return out

    def latest_mlv(self):
        for drive in ("B", "A"):
            best = None
            top = f"{drive}:/DCIM/"
            for is_dir, _, _, name in self.listdir(top):
                if not is_dir or name.startswith("."):
                    continue
                for d, size, ts, fname in self.listdir(top + name + "/"):
                    key = (ts, fname)   # Mdd-hhmm names sort by time as well
                    if not d and fname.upper().endswith(".MLV") and (best is None or key > best[0]):
                        best = (key, f"{top}{name}/{fname}", size)
            if best:
                return best[1], best[2]
        return None, 0

    def call(self, func, *args):
        """call a camera function (CHDK CallFunction): func(args...), <= 6 args"""
        data = struct.pack("<%dI" % (1 + len(args)), func, *[a & 0xFFFFFFFF for a in args])
        return self.transact(PTP_OC_CHDK, [PTP_CHDK_CallFunction], data_out=data)[1]

    def upload(self, camfile, data):
        name = camfile.encode()
        self.transact(PTP_OC_CHDK, [PTP_CHDK_UploadFile],
                      data_out=struct.pack("<I", len(name)) + name + data)

    def fileop(self, op, *paths):
        """op 1 mkdir, 2 rename (src, dst), 3 remove; returns the FIO result"""
        arg = b"\0".join(p.encode() for p in paths)
        self.transact(PTP_OC_CHDK, [PTP_CHDK_TempData, 0], data_out=arg)
        return self.transact(PTP_OC_CHDK, [PTP_CHDK_ML_FileOp, op])[1][0]

    def has_fileop(self):
        try:
            self.transact(PTP_OC_CHDK, [PTP_CHDK_TempData, 0], data_out=b"B:/")
            self.transact(PTP_OC_CHDK, [PTP_CHDK_ML_FileOp, 0])
            return True
        except RuntimeError:
            return False

    def read_file(self, camfile):
        """file contents, or None if missing; empty files via the listing (ML's
        DownloadFile has no data phase for them)"""
        folder, name = camfile.rsplit("/", 1)
        entry = [e for e in self.listdir(folder + "/") if e[3].upper() == name.upper()]
        if not entry:
            return None
        if entry[0][1] == 0:
            return b""
        try:
            return self.download(camfile, progress=False)
        except RuntimeError:
            return None

    def put_verified(self, camfile, data, use_tmp):
        """upload; with use_tmp, write ~UPLOAD.TMP in the same folder, check it,
        then replace camfile, so a broken transfer never leaves a bad file"""
        target = camfile
        if use_tmp:
            target = camfile.rsplit("/", 1)[0] + "/~UPLOAD.TMP"
        for attempt in range(3):
            self.upload(target, data)
            if self.read_file(target) == data:
                break
        else:
            raise RuntimeError(f"{camfile}: read-back differs after 3 uploads")
        if use_tmp:
            self.fileop(3, camfile)             # FAT rename needs a free name
            if self.fileop(2, target, camfile) != 0:
                raise RuntimeError(f"{camfile}: rename failed (new data is in {target})")

    def eos_setprop(self, prop, value):
        self.transact(PTP_OC_EOS_SetDevicePropValueEx, [],
                      data_out=struct.pack("<III", 12, prop, value))

    def eos_events(self):
        """drain Canon's event queue (EOS Utility polls it all the time)"""
        return self.transact(PTP_OC_EOS_GetEvent, [], expect_data=True)[0]

    def remote_lv(self, on, outputs=(3, 2)):
        if on:
            self.transact(PTP_OC_EOS_SetRemoteMode, [1])
            self.transact(PTP_OC_EOS_SetEventMode, [1])
            self.eos_events()
            try:
                self.eos_setprop(PTP_DPC_EOS_EVFMode, 1)
            except RuntimeError:
                pass    # not all models have it
            # camera screen + PC; fall back to PC only
            for value in outputs:
                try:
                    self.eos_setprop(PTP_DPC_EOS_EVFOutputDevice, value)
                    return value
                except RuntimeError:
                    continue
            raise RuntimeError("EVF output device refused")
        else:
            self.eos_setprop(PTP_DPC_EOS_EVFOutputDevice, 0)
            self.transact(PTP_OC_EOS_SetEventMode, [0])
            self.transact(PTP_OC_EOS_SetRemoteMode, [0])

    def close(self):
        try:
            self.transact(PTP_OC_CloseSession, [])
        finally:
            usb.util.release_interface(self.dev, self.intf.bInterfaceNumber)


CARD_SYM = "B:/ML/modules/EOSM2_103.sym"


def card_sym(ptp, name):
    """address of an ML function in the build that runs (the card's .sym,
    read once per connection): never a hard-coded address, which moves
    with every core change and then calls into the middle of another
    function (that hung the camera)"""
    syms = getattr(ptp, "_card_syms", None)
    if syms is None:
        syms = {}
        for line in ptp.read_file(CARD_SYM).decode(errors="replace").splitlines():
            parts = line.split()
            if len(parts) == 2:
                syms[parts[1]] = int(parts[0], 16)
        ptp._card_syms = syms
    if name not in syms:
        sys.exit(f"{name} not in the card's {CARD_SYM}")
    return syms[name]


def sym_address(path, name):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) == 2 and parts[1] == name:
                return int(parts[0], 16)
    # statics are only in the ELF next to the .sym file
    import subprocess
    elf = os.path.join(os.path.dirname(path), "magiclantern")
    try:
        nm = subprocess.run(["arm-none-eabi-nm", elf], capture_output=True, text=True).stdout
    except FileNotFoundError:
        nm = ""
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    sys.exit(f"{name} not in {path} or {elf}")


def read_mailbox(ptp, sym):
    base = sym_address(sym, "ptp_mailbox")
    raw = ptp.read(base, 4 + MAILBOX_SLOTS * 16)
    if struct.unpack_from("<I", raw)[0] != MAILBOX_MAGIC:
        sys.exit("mailbox magic mismatch: is the card's ML build the one --sym points to?")
    slots = []
    for i in range(MAILBOX_SLOTS):
        ptr, lenptr = struct.unpack_from("<II", raw, 4 + 16 * i)
        name = raw[4 + 16 * i + 8:4 + 16 * i + 16].split(b"\0")[0].decode(errors="replace")
        if ptr:
            length = ptp.words(lenptr)[0] if lenptr else 0
            slots.append((i, name, ptr, length))
    return slots


def hexdump(data, addr):
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        print(f"{addr + i:08x}: {chunk.hex(' ')}")


def write_png(path, width, height, rgb):
    """rgb: bytes, 3 per pixel, rows top to bottom"""
    import zlib
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------------------
# crop_rec trace (mailbox slot "crtrace", struct cr_ptp in crop_rec.c):
#   magic, entry_size, entries, ring*, w*, cls_count*, paused*, frz*, frz_caught*,
#   gmt*, cmd, cmd_done, digic_timer_now
# entry: t, cls(u16), lvl(u16), fmt*, a[5], rep  (36 bytes)
# ---------------------------------------------------------------------------
CR_CMD = 40     # offset of cmd in struct cr_ptp


def cr_slot(ptp, sym):
    slots = {name: (ptr, length) for _, name, ptr, length in read_mailbox(ptp, sym)}
    if "crtrace" not in slots:
        sys.exit("no crtrace slot: crop_rec not loaded, or a build without PTP")
    base, length = slots["crtrace"]
    if ptp.words(base)[0] != CR_PTP_MAGIC:
        sys.exit("crtrace: magic mismatch (crop_rec.mo and this script differ)")
    return base, length


def readable(addr):
    """addresses that can't fault: RAM (cached / uncached) and ROM"""
    return (0x1000 <= addr < 0x20000000) or (0x40001000 <= addr < 0x60000000) or addr >= 0xF0000000


class CamStrings:
    """C strings from camera memory, read in aligned 256-byte blocks"""
    def __init__(self, ptp):
        self.ptp, self.blocks = ptp, {}

    def get(self, addr, maxlen=200):
        if not readable(addr):
            return None
        out = b""
        while len(out) < maxlen:
            blk = (addr + len(out)) & ~0xFF
            if blk not in self.blocks:
                n = min(256, 0x100000000 - blk)
                try:
                    self.blocks[blk] = self.ptp.read(blk, n)
                except RuntimeError:
                    return None
            data = self.blocks[blk][(addr + len(out)) - blk:]
            z = data.find(b"\0")
            if z >= 0:
                return (out + data[:z])[:maxlen].decode("latin-1")
            out += data
        return out[:maxlen].decode("latin-1")


import re as _re
_FMT = _re.compile(r"%([-+ #0]*)(\*|\d+)?(?:\.(\d+))?(hh|h|ll|l|z|j|t)?([diouxXcspn%])")


def cformat(fmt, args, strings):
    """printf as DryOS / ML's snprintf would, for 32-bit args"""
    out, ai = [], 0
    pos = 0
    for m in _FMT.finditer(fmt):
        out.append(fmt[pos:m.start()])
        pos = m.end()
        flags, width, prec, length, conv = m.groups()
        if conv == "%":
            out.append("%")
            continue
        if width == "*":
            width = str(args[ai] if ai < len(args) else 0)
            ai += 1
        v = args[ai] if ai < len(args) else 0
        ai += 1
        if conv in "di":
            v = v - (1 << 32) if v & 0x80000000 else v
            s = ("%" + flags + (width or "") + ("." + prec if prec else "") + "d") % v
        elif conv in "uoxX":
            s = ("%" + flags + (width or "") + ("." + prec if prec else "") + conv) % v
        elif conv == "p":
            s = "%x" % v
        elif conv == "c":
            s = chr(v & 0xFF)
        elif conv == "s":
            txt = strings.get(v)
            txt = "(bad %x)" % v if txt is None else txt
            if prec:
                txt = txt[:int(prec)]
            s = ("%" + flags.replace("0", "") + (width or "") + "s") % txt
        else:
            s = ""
        out.append(s)
    out.append(fmt[pos:])
    return "".join(out)


# ---------------------------------------------------------------------------
# crop_rec AF log (mailbox slot "aflog"): header magic, rec_size, n, ring*, w, enabled;
# record: struct aflog_rec in crop_rec.c (64 bytes)
# ---------------------------------------------------------------------------
PDLOG_MAGIC = 0x314C4641    # "AFL1"
PDLOG_REC = struct.Struct("<II34sBBBBBBBB2xiHBBI")
PDLOG_FLAGS = ((1, "crop"), (2, "rec"), (4, "half"), (8, "servo"), (16, "ignored"), (32, "dropped"))
PDLOG_COLS = (["seq", "t_us", "frame", "saf_mode", "flags", "af_mode", "waku_st", "waku_sub",
               "iso", "tv", "av", "focus_pos", "focus_dist_cm", "pd_lens_pos", "pd_extra",
               "valid_areas"] + [f"d{i}" for i in range(10)] + [f"r{i}" for i in range(10)])


def pdlog_header(ptp, sym):
    slots = {name: (ptr, length) for _, name, ptr, length in read_mailbox(ptp, sym)}
    if "aflog" not in slots:
        sys.exit("no aflog slot: crop_rec not loaded, or older build")
    base = slots["aflog"][0]
    magic, rsize, n, ring, w, enabled = ptp.words(base, 6)
    if magic != PDLOG_MAGIC or rsize != PDLOG_REC.size:
        sys.exit("aflog: header mismatch (crop_rec.mo and this script differ)")
    return base, n, ring, w, enabled


def pdlog_rows(raw):
    for off in range(0, len(raw) - PDLOG_REC.size + 1, PDLOG_REC.size):
        (t, seq, pkt, mode, flags, afm, st, sub, iso, tv, av, fpos, fdist, ign, preset,
         frames) = PDLOG_REC.unpack_from(raw, off)
        d = struct.unpack_from(">10h", pkt, 0)
        r = pkt[20:30]
        lens = struct.unpack_from(">H", pkt, 30)[0]
        extra = struct.unpack_from(">H", pkt, 32)[0]
        fl = "+".join(name for bit, name in PDLOG_FLAGS if flags & bit) or "-"
        yield ([seq, t & 0xFFFFF, frames, mode, fl, afm, st, sub, iso, tv, av, fpos, fdist, lens, extra,
                sum(1 for x in r if x)] + list(d) + list(r))


def pdlog_read(ptp, n, ring, start, end):
    """records start..end-1 (absolute numbers), oldest first, reading only those"""
    raw = b""
    i = start
    while i < end:
        slot = i % n
        k = min(end - i, n - slot)
        raw += ptp.read(ring + slot * PDLOG_REC.size, k * PDLOG_REC.size)
        i += k
    return raw


def aflog_cmd(ptp, a):
    import time
    sub = a.args[0] if a.args else ""
    if sub in ("start", "stop", "clear", "save"):
        base, _ = cr_slot(ptp, a.sym)
        cmd = {"start": 5, "stop": 6, "clear": 7, "save": 8}[sub]
        ptp.write32(base + CR_CMD, cmd)
        for _ in range(30):
            time.sleep(0.1)
            if ptp.words(base + CR_CMD)[0] == 0:
                _, n, ring, w, en = pdlog_header(ptp, a.sym)
                print(f"AF log {'on' if en else 'off'}, {w} records"
                      + (" (saved to the next free B:/ML/AFLOGnn.CSV)" if sub == "save" else ""))
                return
        print("not handled yet (crop_rec's polling not running?)")
        return
    _, n, ring, w, enabled = pdlog_header(ptp, a.sym)
    if not ring:
        sys.exit("AF log never started: ptp_ml.py aflog start (or Movie > crop_rec > AF log)")
    start = max(0, w - n, w - a.last if a.last else 0)
    out = open(a.args[0], "w") if a.args else sys.stdout
    out.write(",".join(PDLOG_COLS) + "\n")
    for row in pdlog_rows(pdlog_read(ptp, n, ring, start, w)):
        out.write(",".join(map(str, row)) + "\n")
    if a.args:
        print(f"saved {w - start} records to {a.args[0]} (AF log {'on' if enabled else 'off'})")
    if not a.follow:
        return
    if a.args:
        out.close()
        out = sys.stdout
    try:
        while True:
            time.sleep(0.3)
            _, n, ring, w2, _ = pdlog_header(ptp, a.sym)
            if w2 < w:          # cleared
                w = 0
            if w2 - w > n:      # fell behind
                w = w2 - n
            if w2 > w:
                for row in pdlog_rows(pdlog_read(ptp, n, ring, w, w2)):
                    out.write(",".join(map(str, row)) + "\n")
                out.flush()
                w = w2
            ptp.eos_events() if a.lv else None
    except KeyboardInterrupt:
        pass


def lvci_cmd(ptp, sym):
    """crop_rec.c struct lvci: magic, n_entries, pool_used, 8 x entry (9 words),
    calls, last_caller, last_off, last_size, pool[2048]"""
    slots = {name: (ptr, length) for _, name, ptr, length in read_mailbox(ptp, sym)}
    if "lvci" not in slots:
        sys.exit("no lvci slot: older crop_rec")
    base, length = slots["lvci"]
    raw = ptp.read(base, length)
    magic, n, used = struct.unpack_from("<III", raw)
    if magic != 0x3149434C:
        sys.exit("lvci: magic mismatch")
    calls, caller, loff, lsize = struct.unpack_from("<IIII", raw, 12 + 8 * 36)
    pool = raw[12 + 8 * 36 + 16:]
    print(f"{n} blocks, {calls} writes seen (last from {caller:#x}: +{loff:#x} {lsize} bytes)")
    for i in range(n):
        off, size, rec_ok, stb_ok, rpos, spos, n_rec, n_stb, n_used = struct.unpack_from("<9I", raw, 12 + 36 * i)
        print(f"\n+{off:#05x} ({size} bytes): standby {'yes' if stb_ok else 'no'} ({n_stb} writes), "
              f"recording {'yes' if rec_ok else 'no'} ({n_rec}), substituted {n_used}")
        if not (rec_ok and stb_ok):
            continue
        rec, stb = pool[rpos:rpos + size], pool[spos:spos + size]
        diffs = 0
        for k in range(0, size - size % 4, 4):
            ws, wr = struct.unpack_from("<I", stb, k)[0], struct.unpack_from("<I", rec, k)[0]
            if ws != wr:
                diffs += 1
                print(f"   block +{off + k:#05x}: standby {ws:#010x} ({ws})  recording {wr:#010x} ({wr})")
        for k in range(size - size % 4, size):
            if stb[k] != rec[k]:
                diffs += 1
                print(f"   block +{off + k:#05x} (byte): standby {stb[k]:#04x}  recording {rec[k]:#04x}")
        if not diffs:
            print("   identical")


def crlog(ptp, sym, last=0):
    base, _ = cr_slot(ptp, sym)
    (magic, esize, n, ring, wptr, clsptr, pausedp, frzp, caughtp, gmtp,
     cmd, cmd_done, tnow) = ptp.words(base, 13)
    w = ptp.words(wptr)[0]
    frz, caught, gmt, paused = (ptp.words(p)[0] for p in (frzp, caughtp, gmtp, pausedp))
    start = max(0, w - n)
    if last:
        start = max(start, w - last)
    count = w - start
    raw = ptp.read(ring, n * esize) if count else b""
    cls = ptp.words(clsptr, 256)
    lines = [f"# entries {w} (kept {count}). freeze trace {frz}, caught {caught}, AF trace {gmt}, "
             f"paused {paused}, timer now {tnow:05x}. messages per class:"
             + "".join(f" {c}:{k}" for c, k in enumerate(cls) if k)]
    strings = CamStrings(ptp)
    for i in range(start, w):
        t, c, lvl, fmtp, a0, a1, a2, a3, a4, rep = struct.unpack_from("<IHHIIIIIII", raw, (i % n) * esize)
        fmt = strings.get(fmtp, 300)
        head = f"{t & 0xFFFFF:05x} {-1 if c == 0xFFFF else c:3d}:{lvl} "
        if fmt is None:
            body = f"[raw {fmtp:x}] {a0:x} {a1:x} {a2:x} {a3:x} {a4:x}"
        else:
            body = cformat(fmt, [a0, a1, a2, a3, a4], strings).rstrip("\r\n")
        if rep:
            body += f"  (x{rep + 1})"
        lines.append(head + body)
    return "\n".join(lines) + "\n"


def num_arg(v):
    return int(v, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sym", default=DEFAULT_SYM)
    ap.add_argument("--lv", type=float, default=0, metavar="SECONDS",
                    help="start remote LiveView and wait SECONDS before the command")
    ap.add_argument("cmd", choices=["info", "read", "words", "write", "mailbox", "log", "lv",
                                    "get", "findmlv", "ls", "latest", "console",
                                    "put", "rm", "mkdir", "mv", "install",
                                    "crlog", "crcmd", "saf", "lvs", "aflog", "lvci"])
    ap.add_argument("--follow", action="store_true", help="aflog: keep printing new records")
    ap.add_argument("--lv-out", type=num_arg, default=None, metavar="N",
                    help="EVF output device for --lv (bitmask: 1 camera screen, 2 PC; default 1, then 3, 2)")
    ap.add_argument("--slot", default="saf", help="mailbox slot for the log command")
    ap.add_argument("--last", type=int, default=0, help="crlog: only the newest N entries")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    # M2: output 1 (camera screen only) keeps the LCD live; 3 (screen + PC) left it black
    lv_outputs = (a.lv_out,) if a.lv_out is not None else (1, 3, 2)
    num = lambda s: int(s, 0)

    ptp = PTP()
    lv_on = False
    try:
        if a.lv or a.cmd == "lv":
            import time
            value = ptp.remote_lv(True, lv_outputs)
            lv_on = True
            wait = a.lv or (float(a.args[0]) if a.args else 3)
            t_end = time.time() + wait
            while time.time() < t_end:
                ptp.eos_events()      # keep the session alive
                time.sleep(0.2)
            print(f"remote LiveView on (EVF output {value})", file=sys.stderr)
        if a.cmd == "info":
            d = ptp.dev
            print(f"{d.idVendor:04x}:{d.idProduct:04x} {usb.util.get_string(d, d.iProduct)}")
            print("CHDK protocol version", ptp.chdk_version())
        elif a.cmd == "read":
            addr, length = num(a.args[0]), num(a.args[1])
            data = ptp.read(addr, length)
            if len(a.args) > 2:
                open(a.args[2], "wb").write(data)
                print(f"saved {len(data)} bytes to {a.args[2]}")
            else:
                hexdump(data, addr)
        elif a.cmd == "words":
            addr = num(a.args[0]); count = num(a.args[1]) if len(a.args) > 1 else 1
            for i, w in enumerate(ptp.words(addr, count)):
                print(f"{addr + 4 * i:08x}: {w:08x}")
        elif a.cmd == "write":
            ptp.write32(num(a.args[0]), num(a.args[1]))
            print("ok")
        elif a.cmd == "mailbox":
            for i, name, ptr, length in read_mailbox(ptp, a.sym):
                print(f"slot {i}: {name:8s} ptr {ptr:#010x} len {length}")
        elif a.cmd == "lv":
            print("ML lv =", ptp.words(sym_address(a.sym, "lv"))[0])
        elif a.cmd == "crlog":
            text = crlog(ptp, a.sym, a.last)
            if a.args:
                open(a.args[0], "w").write(text)
                print(f"saved {text.count(chr(10))} lines to {a.args[0]}")
            else:
                sys.stdout.write(text)
        elif a.cmd == "crcmd":
            import time
            names = {"start": 1, "pause": 2, "resume": 3, "savesaf": 4}
            if not a.args or a.args[0] not in names:
                sys.exit("crcmd start|pause|resume|savesaf")
            base, _ = cr_slot(ptp, a.sym)
            cmd = names[a.args[0]]
            ptp.write32(base + CR_CMD, cmd)
            for _ in range(30):     # crop_rec's polling CBR picks it up
                time.sleep(0.1)
                if ptp.words(base + CR_CMD)[0] == 0 and ptp.words(base + CR_CMD + 4)[0] == cmd:
                    print("done")
                    break
            else:
                print("not handled yet (is LiveView / crop_rec's polling running?)")
        elif a.cmd == "lvci":
            lvci_cmd(ptp, a.sym)
        elif a.cmd == "aflog":
            aflog_cmd(ptp, a)
        elif a.cmd == "saf":
            slots = {name: (ptr, length) for _, name, ptr, length in read_mailbox(ptp, a.sym)}
            if "saf" not in slots:
                sys.exit("no saf slot: crop_rec not loaded, or a build without PTP")
            ptr, length = slots["saf"]
            sys.stdout.write(ptp.read(ptr, length).decode(errors="replace") if length else "(empty)\n")
        elif a.cmd == "lvs":
            out = a.args[0] if a.args else "LVS_PTP.BIN"
            hdr = bytearray(64)
            struct.pack_into("<III", hdr, 0, 0x3153564C, LVS_ADDR, LVS_SIZE)
            struct.pack_into("<III", hdr, 12, *ptp.words(0x91C94 + 0x0C, 3))
            # (readout window registers are left 0: MMIO isn't read over PTP)
            open(out, "wb").write(bytes(hdr) + ptp.read(LVS_ADDR, LVS_SIZE))
            print(f"saved {out} (H {ptp.words(LVS_ADDR + 0x3B4)[0] & 0xFFFF}, "
                  f"v {ptp.words(LVS_ADDR + 0x3B0)[0] & 0xFFFF})")
        elif a.cmd == "ls":
            for is_dir, size, ts, name in ptp.listdir(a.args[0]):
                print(f"{'D' if is_dir else ' '} {size:12d} {ts:10d} {name}")
        elif a.cmd in ("get", "latest"):
            import time
            if a.cmd == "latest":
                camfile, size = ptp.latest_mlv()
                if not camfile:
                    sys.exit("no MLV files found in B:/DCIM or A:/DCIM")
                print(f"newest: {camfile} ({size >> 20} MB)")
                a.args = [camfile] + a.args
            t = time.time()
            local = a.args[1] if len(a.args) > 1 else a.args[0].replace("\\", "/").split("/")[-1]
            size = ptp.file_size64(a.args[0])
            if size is not None:
                # ranged reads: any size (DownloadFile stops at 2 GB)
                with open(local, "wb") as out:
                    n = ptp.download_ranged(a.args[0], out, size)
            else:
                data = ptp.download(a.args[0])
                open(local, "wb").write(data)
                n = len(data)
            dt = time.time() - t
            print(f"saved {n} bytes to {local} ({n / dt / 1e6:.1f} MB/s)")
        elif a.cmd == "findmlv":
            import re
            found = set()
            for base in range(0x00100000, 0x01000000, CHUNK):
                for m in re.finditer(rb"[A-Z]:/DCIM/[0-9A-Z_]{8}/[A-Z0-9_-]{4,8}\.MLV", ptp.read(base, CHUNK)):
                    found.add(m.group().decode())
            for f in sorted(found):
                print(f)
        elif a.cmd == "put":
            ptp.put_verified(a.args[1], open(a.args[0], "rb").read(), ptp.has_fileop())
            print("ok")
        elif a.cmd in ("rm", "mkdir", "mv"):
            op = {"mkdir": 1, "mv": 2, "rm": 3}[a.cmd]
            print("result", ptp.fileop(op, *a.args))
        elif a.cmd == "install":
            import zipfile
            if not a.args:
                sys.exit("install BUILD.zip (from make zip in platform/EOSM2.103)")
            z = zipfile.ZipFile(a.args[0])
            files = [i for i in z.infolist() if not i.filename.endswith("/")]
            files.sort(key=lambda i: i.filename.lower() == "autoexec.bin")   # last
            use_tmp = ptp.has_fileop()
            if not use_tmp:
                print("camera's ML has no file operations yet: direct uploads (verified)")
                print("  if the transfer breaks, the card may need fixing in a reader")
            dirs = sorted({"B:/" + i.filename.rsplit("/", 1)[0] for i in files if "/" in i.filename})
            for d in dirs:
                if use_tmp:
                    ptp.fileop(1, d)      # fails harmlessly if it exists
            changed = 0
            for i in files:
                camfile = "B:/" + i.filename
                data = z.read(i)
                if ptp.read_file(camfile) == data:
                    continue
                print(f"  {camfile} ({len(data)} bytes)")
                ptp.put_verified(camfile, data, use_tmp)
                changed += 1
            # a module hidden in an older build stays hidden while its .hid
            # marker is on the card: drop markers the build no longer has
            hid = {i.filename.lower() for i in files if i.filename.lower().endswith(".hid")}
            if use_tmp and any(f.startswith("ml/modules/") for f in hid):
                for is_dir, size, ts, name in ptp.listdir("B:/ML/modules/"):
                    if not is_dir and name.lower().endswith(".hid") and "ml/modules/" + name.lower() not in hid:
                        print(f"  removing stale B:/ML/modules/{name}")
                        ptp.fileop(3, "B:/ML/modules/" + name)
                        changed += 1
            print(f"installed: {changed} of {len(files)} files changed; restart the camera to run it")
        elif a.cmd == "console":
            W, H = 80, 21           # src/console.c
            buf = ptp.read(sym_address(a.sym, "console_buffer"), W * H)
            idx = ptp.words(sym_address(a.sym, "console_buffer_index"))[0]
            start = (idx // W + 1) % H      # oldest line first
            for i in range(H):
                line = buf[((start + i) % H) * W:((start + i) % H + 1) * W]
                text = line.decode("latin-1").rstrip()
                if text:
                    print(text)
        elif a.cmd == "log":
            slots = {name: (ptr, length) for _, name, ptr, length in read_mailbox(ptp, a.sym)}
            if a.slot not in slots:
                sys.exit(f"no {a.slot} slot")
            ptr, length = slots[a.slot]
            data = ptp.read(ptr, length)
            if a.args:
                open(a.args[0], "wb").write(data)
                print(f"saved {length} bytes to {a.args[0]}")
            else:
                sys.stdout.write(data.decode(errors="replace"))
    finally:
        if lv_on:
            try:
                ptp.remote_lv(False)
            except RuntimeError as e:
                print("remote LiveView off:", e, file=sys.stderr)
        ptp.close()


if __name__ == "__main__":
    main()
