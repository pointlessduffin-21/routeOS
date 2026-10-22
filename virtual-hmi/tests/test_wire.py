"""Canonical host codec checks against stored golden vectors."""
import json
from pathlib import Path
import sys
import pytest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from protocol import rvdp as W

VECTORS = json.loads((ROOT / 'protocol/vectors/v1.json').read_text())

def test_golden_messages():
    assert W.pack(1, 0, W.hello()).hex() == VECTORS['hello_view']
    assert W.read_display(W.unpack(bytes.fromhex(VECTORS['display_rgb_row'])).payload) == (3, 1, 1, False)
    assert W.read_rectangle(W.unpack(bytes.fromhex(VECTORS['rect_rgb_row'])).payload, 3, 1)[-1] == bytes.fromhex('f80007e0001f')
    assert W.read_pointer(W.unpack(bytes.fromhex(VECTORS['pointer_down'])).payload, 240, 135) == (120, 68, 1, 1)

@pytest.mark.parametrize('name', ['bad_magic', 'unsupported_version', 'truncated', 'oversized_length'])
def test_bad_vectors(name):
    with pytest.raises(ValueError):
        W.unpack(bytes.fromhex(VECTORS[name]))

@pytest.mark.parametrize('x,y,down,event', [(240, 0, 0, 0), (0, 135, 1, 1), (0, 0, 2, 1), (0, 0, 1, 3)])
def test_invalid_pointer(x, y, down, event):
    with pytest.raises(ValueError):
        W.read_pointer(W.POINTER.pack(x, y, down, event), 240, 135)

def test_fuzz_malformed_lengths():
    import random
    r = random.Random(20261022)
    for size in range(512):
        with pytest.raises(ValueError):
            W.unpack(r.randbytes(size))
