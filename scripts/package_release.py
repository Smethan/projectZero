"""Create board-specific, checksummed release ZIPs from completed IDF builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'^#define JANOS_VERSION "(\d+\.\d+\.\d+)"',
                    (ROOT/'ESP32C5/main/main.c').read_text(), re.M).group(1)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--check-version', action='store_true')
    p.add_argument('--board', choices=['wroom', 'xiao'])
    p.add_argument('--source', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    ref = os.environ.get('GITHUB_REF', '')
    if ref.startswith('refs/tags/') and ref != 'refs/tags/v'+VERSION:
        raise SystemExit('Release tag must equal v'+VERSION)
    if args.check_version:
        print('Version:', VERSION)
        return
    if not all((args.board, args.source, args.output)):
        p.error('--board, --source and --output required')
    build = args.source/'build'
    config = (args.source/'sdkconfig').read_text()
    assert ('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' in config) == (args.board == 'xiao')
    suffix = '-xiao' if args.board == 'xiao' else ''
    app = 'projectZerobyLOCOSP'+suffix+'.bin'
    sources = {'bootloader.bin': build/'bootloader/bootloader.bin',
               'partition-table.bin': build/'partition_table/partition-table.bin',
               'ota_data_initial.bin': build/'ota_data_initial.bin',
               app: build/'projectZerobyLOCOSP.bin'}
    offsets = dict(zip(sources, ['0x2000','0x8000','0xf000','0x20000']))
    flash = json.loads((build/'flasher_args.json').read_text())['flash_files']
    assert set(flash) == set(offsets.values()), flash
    description = json.loads((build/'project_description.json').read_text())
    assert description['project_version'] == VERSION, description['project_version']
    ota_project = re.search(r'^#define OTA_PROJECT_NAME "([^"]+)"',
                           (ROOT/'ESP32C5/main/main.c').read_text(), re.M).group(1)
    assert description['project_name'] == ota_project, 'OTA must accept the built project name'
    manifest = dict(format=1, repository='Smethan/projectZero', board=args.board,
                    chip='esp32c5', version=VERSION,
                    source_commit=subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip(),
                    files={name:dict(offset=offsets[name], size=path.stat().st_size,
                        sha256=hashlib.sha256(path.read_bytes()).hexdigest()) for name,path in sources.items()})
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output/('projectZerobyLOCOSP'+suffix+'-'+VERSION+'.zip')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, path in sources.items():
            z.write(path, name)
        z.writestr('manifest.json', json.dumps(manifest, indent=2)+'\n')
    shutil.copy2(sources[app], args.output/app)
    print(target)

if __name__ == '__main__':
    main()
