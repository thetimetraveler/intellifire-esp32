#!/usr/bin/env bash
# Capture and decode the ECM <-> WFM serial link.
#
# LISTEN-ONLY BY CONSTRUCTION: this script opens no transmit path. sigrok-cli's UART
# decoder is receive-only, and the analyzer is an input device. Nothing can be sent
# to the fireplace by this script.
#
# Wiring (see docs/ANALYZER_SETUP.md):
#   analyzer GND -> ECM pin 2 or 8   (CONFIRMED grounds — connect FIRST, remove LAST)
#   CH0          -> ECM pin 6        (ECM TX candidate)
#   CH1          -> ECM pin 4        (second data candidate)
#   CH2          -> ECM pin 5        (idle-high candidate; optional)
#   NEVER        -> pin 3            (+5 V supply — will damage a 5.25 V-limited analyzer)
#
# Usage:
#   ./ecm_capture.sh                 # 30 s capture on CH0/CH1, decode both
#   ./ecm_capture.sh 120             # 120 s capture
#   ./ecm_capture.sh 60 0,1,2        # include CH2

set -euo pipefail

DURATION="${1:-30}"
CHANNELS="${2:-D0,D1}"
BAUD=9600
# NOTE: sigrok-cli must be given an ABSOLUTE path. The ~/.local/bin/sigrok-cli
# wrapper execs a launcher that cd's into the app bundle, so a relative path
# fails with "Not a regular file". Resolve OUTDIR to absolute here.
OUTDIR="${OUTDIR:-captures}"
STAMP="$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUTDIR"
OUTDIR="$(cd "$OUTDIR" && pwd)"
SR="${OUTDIR}/ecm-${STAMP}.sr"

echo "=== fireplace ECM link capture ==="
echo "duration : ${DURATION}s"
echo "channels : ${CHANNELS}"
echo "baud     : ${BAUD}-8-N-1"
echo "output   : ${SR}"
echo
echo "REMINDERS"
echo "  - analyzer GND on pin 2 or 8, connected FIRST"
echo "  - NEVER clip pin 3 (+5 V supply)"
echo "  - selector in IFT-REM means the fireplace CAN IGNITE"
echo "  - RF remote batteries should be OUT"
echo
read -r -p "Press Enter when wired and ready (Ctrl-C to abort)... " _

echo "--- scanning for device ---"
if ! sigrok-cli --scan; then
  echo "ERROR: no device found. Check the USB connection and the fx2lafw driver." >&2
  exit 1
fi

echo "--- capturing ${DURATION}s at 1 MHz ---"
sigrok-cli \
  --driver fx2lafw \
  --config samplerate=1m \
  --channels "$CHANNELS" \
  --time "${DURATION}s" \
  --output-file "$SR"

echo "--- captured: $(du -h "$SR" | cut -f1) ---"
echo

echo "=========== DECODE ==========="
# Decode UART on each channel, at the hypothesised 9600-8-N-1.
# The flag is -P / --protocol-decoders (plural). The singular --protocol-decoder
# is not a valid option and fails with "Unknown option".
for ch in ${CHANNELS//,/ }; do
  echo "--- UART on ${ch} @ ${BAUD} ---"
  sigrok-cli -i "$SR" \
    -P uart:baudrate=${BAUD}:rx="${ch}" \
    -A uart=rx-data \
    2>&1 | head -80 || true
  echo
done

echo "=========== RAW EDGE CHECK ==========="
echo "If the decoder produced nothing, confirm the channel is actually toggling:"
for ch in ${CHANNELS//,/ }; do
  printf "  %s : " "$ch"
  sigrok-cli -i "$SR" --channels "$ch" --samples 20 --show 2>/dev/null | head -2 || echo "(no data)"
done
echo
echo "=========== BYTES LOOKING FOR ==========="
echo "  start byte 0x4B, then device/seq/command/payload, MODBUS CRC-16 little-endian"
echo "  (confirmed against the 1 MHz capture — see docs/FRAME_FORMAT.md)"
echo
echo "Saved: $SR"
echo "Analyse further with:  sigrok-cli -i $SR -P uart:baudrate=9600:rx=D0 -A uart=rx-data"
