#!/usr/bin/env python3
import sys
import os
import glob
import re

# Transparently switch to ESP-IDF Python if pyserial is not installed in current interpreter
try:
    import serial
except ImportError:
    candidate_envs = sorted(glob.glob(os.path.expanduser('~/.espressif/python_env/*/bin/python')), reverse=True)
    switched = False
    for py in candidate_envs:
        if os.path.isfile(py) and os.access(py, os.X_OK):
            os.execv(py, [py] + sys.argv)
            switched = True
            break
    if not switched:
        print("Error: 'pyserial' is not installed in your Python environment.")
        print("Please run: . $IDF_PATH/export.sh  OR  pip install pyserial")
        sys.exit(1)

import time

def capture_dataset(port, output_file, target_rows):
    print(f"Connecting to {port}...")
    ser = serial.Serial(port, 115200, timeout=2)

    rows_captured = 0
    print(f"Capturing {target_rows} rows into '{output_file}'...")

    pattern = re.compile(r'(\d+),(-?\d+),(\d+),(\d+),(\d+),(\d+),(\d+)')

    with open(output_file, 'w', encoding='utf-8') as f:
        # Write clean header on line 1
        f.write("timestamp,rssi,local_ping,public_ping,dns_time,http_status,http_time\n")
        f.flush()

        while rows_captured < target_rows:
            try:
                raw = ser.readline().decode('utf-8', errors='ignore')
            except Exception as e:
                print(f"Serial read error: {e}")
                continue

            if not raw:
                continue

            # Look for 7 CSV values in line
            m = pattern.search(raw)
            if m:
                ts = int(m.group(1))
                rssi = int(m.group(2))
                lp = int(m.group(3))
                pp = int(m.group(4))
                dns = int(m.group(5))
                status = int(m.group(6))
                ht = int(m.group(7))

                f.write(f"{ts},{rssi},{lp},{pp},{dns},{status},{ht}\n")
                f.flush()
                rows_captured += 1

                if rows_captured % 10 == 0 or rows_captured == target_rows:
                    print(f"[{rows_captured}/{target_rows}] Sample: {ts},{rssi},{lp},{pp},{dns},{status},{ht}")

    ser.close()
    print(f"Capture complete! Saved {rows_captured} rows to '{output_file}'.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 scripts/capture_csv.py <output_file.csv> <target_rows> [port]")
        sys.exit(1)

    out_file = sys.argv[1]
    count = int(sys.argv[2])
    serial_port = sys.argv[3] if len(sys.argv) > 3 else "/dev/ttyACM0"
    capture_dataset(serial_port, out_file, count)
