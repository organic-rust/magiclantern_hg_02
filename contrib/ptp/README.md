# PTP over USB (EOS M2)

ML's CHDK-style PTP operation (src/ptp-chdk.c) plus the extensions from the
M6 II tree: directory listing, 64-bit ranged reads, mkdir / rename / remove
and a mailbox of named buffers. Enabled for EOSM2 in
platform/EOSM2.103/Makefile.setup.default.

Host side: `ptp_ml.py` (needs pyusb; run with sudo or add a udev rule for
vendor 04a9). Copy autoexec.bin, EOSM2_103.sym and the modules from the same
build to the card: the mailbox address comes from platform/EOSM2.103/magiclantern.sym.

    ./ptp_ml.py info               # CHDK protocol version
    ./ptp_ml.py mailbox            # slots: crtrace, saf
    ./ptp_ml.py crcmd start        # start crop_rec's Freeze trace
    ./ptp_ml.py crlog FRZ.TXT      # read the trace (also after a LiveView freeze)
    ./ptp_ml.py saf                # Hybrid AF (SAF) info
    ./ptp_ml.py lvs LVS_M.BIN      # Canon's LV settings block
    ./ptp_ml.py ls B:/ML/          # card files; get / put / install
    ./ptp_ml.py console            # ML console text

Connecting USB normally leaves LiveView; `--lv SECONDS` starts Canon's remote
LiveView first (EOS Utility style) - not yet tried on the M2.
