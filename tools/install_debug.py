"""Run fm1-dock's fm1_install.py with logging: every port list change and every request, with times.
   out/venv/Scripts/python tools/install_debug.py PACKAGE.fwsc"""
import sys, time, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "third_party", "fm1-dock", "tools"))
import fm1_install as fi

T0 = time.monotonic()
def log(*a):
    print(f"[{time.monotonic() - T0:7.2f}]", *a, flush=True)

_names = fi.MidoBackend.input_names
last = [None]
def names(self):
    n = _names(self)
    if n != last[0]:
        log("ports in:", n, "out:", self.mido.get_output_names())
        last[0] = n
    return n
fi.MidoBackend.input_names = names

_parse = fi.parse_request
def parse(pkt):
    r = _parse(pkt)
    if r:
        log(f"request fl={r[0]:#x} addr={r[1]:#x} n={r[2]}")
    else:
        log("other sysex", pkt[:16].hex())
    return r
fi.parse_request = parse

_send = fi.MidoLink.send
def send(self, pkt):
    ok = _send(self, pkt)
    if not ok:
        log("send FAILED")
    return ok
fi.MidoLink.send = send

_lost = fi.MidoLink.lost
fi.MidoLink.lost = property(lambda self: (log("lost? port", self.name), _lost.fget(self))[1])

args = sys.argv[1:] + ["--yes"]
sys.exit(fi.main(args))
