#!/usr/bin/env python3
import sys
import os
import glob

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
    ser = serial.Serial(port, 115200, timeout=3)

    rows_captured = 0
    print(f"Capturing {target_rows} rows into '{output_file}'...")

    with open(output_file, 'w', encoding='utf-8') as f:
        # Write clean header on line 1
        f.write("timestamp,rssi,local_ping,public_ping,dns_time,http_status,http_time\n")
        f.flush()

        while rows_captured < target_rows:
            try:
                line = ser.readline().decode('utf-8', errors='ignore').strip()
            except Exception as e:
                print(f"Serial read error: {e}")
                continue

            if not line:
                continue

            # Skip header if printed by device
            if "timestamp,rssi,local_ping" in line:
                continue

            # Parse data line
            parts = line.split(',')
            if len(parts) == 7:
                try:
                    ts = int(parts[0])
                    rssi = int(parts[1])
                    lp = int(parts[2])
                    pp = int(parts[3])
                    dns = int(parts[4])
                    status = int(parts[5])
                    ht = int(parts[6])

                    # Write clean row
                    f.write(f"{ts},{rssi},{lp},{pp},{dns},{status},{ht}\n")
                    f.flush()
                    rows_captured += 1

                    if rows_captured % 10 == 0 or rows_captured == target_rows:
                        print(f"[{rows_captured}/{target_rows}] Sample: {ts},{rssi},{lp},{pp},{dns},{status},{ht}")
                except ValueError:
                    # Ignore non-numeric debug logs
                    continue

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
