"""Host tests for the shared driver. Supply the pinned epdiy checkout for waveform/rotation checks."""
from pathlib import Path
import argparse
import ast
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--epdiy-source', type=Path, required=True)
args, remaining = parser.parse_known_args()
EPDIY = args.epdiy_source
DRIVER = (ROOT/'components/t5_epaper/t5_epaper.h').read_text()

def method(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace(' override', '')

def run_cpp(source):
    with tempfile.TemporaryDirectory() as tmp:
        cpp, exe = Path(tmp)/'test.cpp', Path(tmp)/'test'
        cpp.write_text(source)
        subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

class DriverTests(unittest.TestCase):
    def test_checked_recovery_and_manual_deghost(self):
        methods = [method(DRIVER, s) for s in (
            'void update() override', 'void clean_screen()', 'void deep_clean()',
            'bool solid_pass_', 'void draw_failed_()', 'bool epd_powergood_()')]
        fixture = (ROOT/'tests/display_recovery_test.cpp').read_text()
        run_cpp(fixture.replace('// INJECT_METHODS','\n'.join(methods)))

    def test_white_recovery_waveform(self):
        wave = (EPDIY/'src/waveforms/epdiy_ED047TC1.h').read_text()
        array = re.search(r'epd_wp_epdiy_ED047TC1_1_0_data\[.*?= (.*?);', wave).group(1)
        for phase in ast.literal_eval(array.replace('{','[').replace('}',']')):
            white = phase[15]
            pulses = [(white[s//4] >> (6-2*(s%4))) & 3 for s in range(16)]
            self.assertEqual(pulses[:15], [2]*15)
            self.assertEqual(pulses[15], 0)

    def test_touch_matches_actual_epdiy_pixel_rotation(self):
        rotate = method((EPDIY/'src/epdiy.c').read_text(), 'Coord_xy _rotate(')
        transform = method(DRIVER, 'bool transform_touch_(')
        fixture = (ROOT/'tests/touch_rotation_test.cpp').read_text()
        run_cpp(fixture.replace('// INJECT_EPD_ROTATE',rotate).replace('// INJECT_TOUCH',transform))

if __name__ == '__main__': unittest.main(argv=['test_driver.py',*remaining],verbosity=2)
