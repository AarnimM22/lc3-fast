"""Read RTT channel 0 through the selected J-Link core's memory interface.

Used because JLinkRTTLogger 9.24a fails to find this nRF5340 NET block even
though J-Link Commander reads it correctly. No CPU halt/reset is performed.
"""
import argparse
import ctypes as C
import struct
import sys
import time
from pathlib import Path

# PowerShell's inherited legacy code page cannot print every RTT byte. The
# capture remains raw bytes; make console rendering robust to damaged text.
sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')

p = argparse.ArgumentParser()
p.add_argument('--serial', type=int, required=True)
p.add_argument('--device', required=True)
p.add_argument('--address', type=lambda s: int(s, 0), required=True)
p.add_argument('--seconds', type=int, default=60)
p.add_argument('--output', type=Path, required=True)
p.add_argument('--stop-on', default='', help='Finish after this text appears in the capture')
a = p.parse_args()
targets = {
    (683088082, 'NRF52840_XXAA'): (0x20000000, 0x40000),
    (1050038165, 'NRF5340_XXAA_NET'): (0x21000000, 0x10000),
    (1050038165, 'NRF5340_XXAA_APP'): (0x20000000, 0x70000),
    (1051883190, 'nRF54LM20A_M33'): (0x20000000, 0x80000),
    (1051802784, 'nRF54LM20A_M33'): (0x20000000, 0x80000),
}
if (a.serial, a.device) not in targets:
    raise SystemExit('Unrecognized bench target')
lo, ram_size = targets[(a.serial, a.device)]
hi = lo + ram_size
d = C.CDLL(r'C:\Program Files\SEGGER\JLink_V924a\JLink_x64.dll')
d.JLINKARM_Open.restype = C.c_char_p
d.JLINKARM_EMU_SelectByUSBSN(a.serial)
err = d.JLINKARM_Open()
if err: raise RuntimeError(err)

def read(addr, n):
    assert lo <= addr and addr+n <= hi
    buf = (C.c_ubyte*n)()
    rc = d.JLINKARM_ReadMemU8(addr, n, buf, None)
    if rc != n: raise RuntimeError(f'Read {addr:x}: {rc}/{n}')
    return bytes(buf)

def read_control_block(addr):
    # WrOff is changed by the running CPU. Byte reads can tear its low/high
    # bytes at a carry and falsely look like a full ring-buffer wrap.
    assert lo <= addr and addr+48 <= hi and addr % 4 == 0
    words = (C.c_uint32*12)()
    rc = d.JLINKARM_ReadMemU32(addr, 12, words, None)
    if rc != 12: raise RuntimeError(f'Read control block {addr:x}: {rc}/12 words')
    return bytes(words)

try:
    msg = C.create_string_buffer(1024)
    rc = d.JLINKARM_ExecCommand(f'Device = {a.device}'.encode(), msg, len(msg))
    if rc: raise RuntimeError(msg.value)
    d.JLINKARM_TIF_Select(1)
    d.JLINKARM_SetSpeed(4000)
    if d.JLINKARM_Connect() < 0: raise RuntimeError('Connect failed')
    end = time.monotonic()+a.seconds
    needle = a.stop_on.encode()
    tail = b''
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with a.output.open('ab', buffering=0) as out:
        while time.monotonic()<end:
            cb = read_control_block(a.address)
            if not cb.startswith(b'SEGGER RTT'):
                time.sleep(.05)
                continue
            name, buf, size, wr, rd, flags = struct.unpack_from('<6I', cb, 24)
            if not (0<size<=65536 and wr<size and rd<size):
                raise RuntimeError(f'Invalid RTT descriptor at {a.address:#x}: {cb.hex()}')
            if wr != rd:
                data = read(buf+rd, wr-rd) if wr>rd else read(buf+rd, size-rd)+read(buf, wr)
                out.write(data)
                rc=d.JLINKARM_WriteU32(a.address+40, wr)
                if rc<0: raise RuntimeError(f'RTT RdOff write failed: {rc}')
                print(data.decode('utf-8', errors='replace'),end='',flush=True)
                if needle:
                    # printk can be read halfway through a result line. Wait
                    # for its newline before stopping, preserving all fields.
                    pending = tail+data
                    lines = pending.split(b'\n')
                    if any(needle in line for line in lines[:-1]):
                        break
                    tail = lines[-1][-65536:]
            time.sleep(.02)
finally:
    d.JLINKARM_Close()
