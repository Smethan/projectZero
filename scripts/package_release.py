"""Create board-specific, checksummed release ZIPs from completed IDF builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.search(r'^#define JANOS_VERSION "(\d+\.\d+\.\d+)"',
                    (ROOT/'ESP32C5/main/main.c').read_text(), re.M).group(1)

PROFILES = {
    'wroom': (8 * 1024 * 1024, 0x3E0000, '', 'uart', ['usb-ota', 'rom']),
    'xiao': (8 * 1024 * 1024, 0x3E0000, '-xiao', 'usb-serial-jtag', ['usb-ota', 'rom']),
    'waveshare-zero': (4 * 1024 * 1024, 0x3D0000, '-waveshare-zero', 'usb-serial-jtag', ['rom']),
}


def build_source_hash(source):
    """Hash the disposable inputs, including its generated board config."""
    digest = hashlib.sha256()
    for path in sorted(source.rglob('*')):
        relative = path.relative_to(source)
        if not path.is_file() or any(part in ('build', '.git', '__pycache__') for part in relative.parts):
            continue
        if path.suffix in ('.bin', '.log', '.pyc'):
            continue
        digest.update(relative.as_posix().encode() + b'\0')
        digest.update(path.read_bytes())
        digest.update(b'\0')
    return digest.hexdigest()


def partitions_from_binary(path):
    partitions = []
    raw = path.read_bytes()
    for offset in range(0, len(raw) - 31, 32):
        magic, kind, subtype, address, size, name, flags = struct.unpack_from('<HBBII16sI', raw, offset)
        if magic != 0x50AA:
            break  # MD5/footer are not partition entries.
        partitions.append(dict(name=name.split(b'\0', 1)[0].decode('ascii'),
                               type=kind, subtype=subtype, offset=address, size=size, flags=flags))
    if not partitions:
        raise ValueError('Generated partition table has no entries')
    return partitions


def validate_build(board, source):
    flash_bytes, app_limit, suffix, console, updates = PROFILES[board]
    build = source / 'build'
    config = (source / 'sdkconfig').read_text()
    native_usb = 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' in config
    zero = 'CONFIG_PROJECTZERO_BOARD_WAVESHARE_ZERO=y' in config
    if native_usb != (board != 'wroom') or zero != (board == 'waveshare-zero'):
        raise ValueError('Build console/board profile does not match package')
    if f'CONFIG_ESPTOOLPY_FLASHSIZE="{flash_bytes // 1048576}MB"' not in config:
        raise ValueError('Build flash geometry does not match board')
    if zero and ('CONFIG_SPIRAM=y' in config or 'CONFIG_COMPILER_OPTIMIZATION_SIZE=y' not in config):
        raise ValueError('Zero requires no PSRAM and size optimization')
    if board == 'xiao' and ('CONFIG_SPIRAM=y' not in config
                            or 'CONFIG_SPIRAM_BOOT_INIT=y' not in config
                            or 'CONFIG_SPIRAM_IGNORE_NOTFOUND=y' not in config):
        raise ValueError('XIAO requires boot PSRAM init and tolerance for an absent PSRAM chip')
    if zero and ('CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=y' not in config
                 or any(option+'=y' in config for option in (
                     'CONFIG_ESP_WIFI_IRAM_OPT', 'CONFIG_ESP_WIFI_EXTRA_IRAM_OPT',
                     'CONFIG_ESP_WIFI_RX_IRAM_OPT'))):
        raise ValueError('Zero requires assertions and disabled optional WiFi IRAM paths')
    flash = json.loads((build / 'flasher_args.json').read_text())['flash_files']
    partitions = partitions_from_binary(build / 'partition_table/partition-table.bin')
    apps = [part for part in partitions if part['type'] == 0]
    expected_apps = [(0, 0x20000, 0x3E0000)] if zero else [
        (0x10, 0x20000, 0x3F0000), (0x11, 0x410000, 0x3F0000)]
    if [(p['subtype'], p['offset'], p['size']) for p in apps] != expected_apps:
        raise ValueError('Generated application layout differs from board contract')
    nvs = [p for p in partitions if p['type'] == 1 and p['subtype'] == 2]
    if len(nvs) != 1 or (nvs[0]['offset'], nvs[0]['size']) != (0x9000, 0x6000):
        raise ValueError('NVS preservation layout differs from contract')
    for part in partitions:
        if part['offset'] + part['size'] > flash_bytes:
            raise ValueError('Partition exceeds physical flash')
    ordered = sorted(partitions, key=lambda p:p['offset'])
    if any(a['offset']+a['size'] > b['offset'] for a,b in zip(ordered,ordered[1:])):
        raise ValueError('Generated partitions overlap')
    app = build / 'projectZerobyLOCOSP.bin'
    if app.stat().st_size > app_limit:
        raise ValueError(f'Application exceeds release size limit {app_limit}')
    sources = {}
    # Use generated flash metadata rather than assuming component presence.
    known = {0x2000: 'bootloader.bin', 0x8000: 'partition-table.bin',
             0xF000: 'ota_data_initial.bin', 0x20000: 'projectZerobyLOCOSP' + suffix + '.bin'}
    intervals = []
    for address_text, filename in flash.items():
        address = int(address_text, 0)
        if address not in known:
            raise ValueError(f'Unexpected full-install write at {address_text}')
        path = build / filename
        size = path.stat().st_size
        end = address + size
        if end > flash_bytes or any(address < hi and end > lo for lo, hi in intervals):
            raise ValueError('Flashed components overlap or exceed flash')
        if address < 0xF000 and end > 0x9000:
            raise ValueError('Full install would overwrite NVS')
        region_end = {0x2000: 0x8000, 0x8000: 0x9000, 0xF000: 0x11000,
                      0x20000: 0x20000 + apps[0]['size']}[address]
        if end > region_end:
            raise ValueError('Component exceeds its reserved region')
        intervals.append((address, end))
        sources[known[address]] = (address_text, path)
    if not {0x2000, 0x8000, 0x20000}.issubset(int(v[0], 0) for v in sources.values()):
        raise ValueError('Required bootloader, partition and application components missing')
    return sources, dict(flash_bytes=flash_bytes, application_limit=app_limit, console=console,
                        psram=not zero, partitions=partitions, updates=updates,
                        application_offset=0x20000,
                        application_size=apps[0]['size'], nvs_preserved=True)

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--check-version', action='store_true')
    p.add_argument('--board', choices=list(PROFILES))
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
    sources_with_offsets, geometry = validate_build(args.board, args.source)
    suffix = PROFILES[args.board][2]
    app = 'projectZerobyLOCOSP'+suffix+'.bin'
    description = json.loads((build/'project_description.json').read_text())
    assert description['project_version'] == VERSION, description['project_version']
    ota_project = re.search(r'^#define OTA_PROJECT_NAME "([^"]+)"',
                           (ROOT/'ESP32C5/main/main.c').read_text(), re.M).group(1)
    assert description['project_name'] == ota_project, 'OTA must accept the built project name'
    manifest = dict(format=2, repository='Smethan/projectZero', board=args.board,
                    chip='esp32c5', version=VERSION,
                    **geometry,
                    source_commit=subprocess.check_output(['git','rev-parse','HEAD'], cwd=ROOT, text=True).strip(),
                    source_dirty=bool(subprocess.check_output(
                        ['git', 'status', '--porcelain', '--', 'ESP32C5', 'scripts/package_release.py'],
                        cwd=ROOT, text=True).strip()),
                    build_source_sha256=build_source_hash(args.source),
                    files={name:dict(offset=offset, size=path.stat().st_size,
                        sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                           for name,(offset,path) in sources_with_offsets.items()})
    args.output.mkdir(parents=True, exist_ok=True)
    target = args.output/('projectZerobyLOCOSP'+suffix+'-'+VERSION+'.zip')
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, (_, path) in sources_with_offsets.items():
            z.write(path, name)
        z.writestr('manifest.json', json.dumps(manifest, indent=2)+'\n')
    shutil.copy2(sources_with_offsets[app][1], args.output/app)
    print(target)

if __name__ == '__main__':
    main()
