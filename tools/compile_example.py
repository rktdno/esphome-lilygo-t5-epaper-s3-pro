"""Compile the public example against this checkout, with dummy WiFi and no secrets file."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT/'example.yaml').read_text()
source = 'source: github://rktdno/esphome-lilygo-t5-epaper-s3-pro'
assert source in text, 'Update the local-source override to match example.yaml'
text = text.replace(source, 'source: {type: local, path: ' + json.dumps(str(ROOT/'components')) + '}')
text = text.replace('!secret wifi_ssid', '"validation-only"').replace('!secret wifi_password', '"not-a-real-network"')
assert '!secret' not in text, 'Add dummy values for any new example secrets'
text = text.replace('esphome:\n', 'esphome:\n  build_path: ' + json.dumps(str(ROOT/'.esphome/build/standalone-check')) + '\n', 1)
cli = shutil.which('esphome') or str(Path.home()/'.local/bin/esphome')
with tempfile.TemporaryDirectory() as tmp:
    config = Path(tmp)/'example.yaml'
    config.write_text(text)
    subprocess.run([cli,'compile',str(config)],check=True)
