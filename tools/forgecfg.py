"""forge.json: the project's values for every PC-side tool (docs/PROTOCOL.md). Standard library only.

    import forgecfg
    cfg = forgecfg.load()          # forge.json found by walking up from this file's folder
    root = cfg['_root']            # the folder that holds forge.json (the repository root)
    dev = forgecfg.devloop(cfg)    # <root>/.devloop, created if needed

Defaults fill what forge.json leaves out, so an older forge.json keeps working when a tool learns a new value.
"""
import json
import os

DEFAULTS = {
    'app': 'espforge',
    'chip': 'esp32s3',
    'build_dir': 'build',
    'port': '',
    'baud': 460800,
    'monitor_baud': 115200,
    'https_port': 443,
    'screen': {'w': 466, 'h': 466, 'shape': 'rect'},
    'screens': [],
    'screens_not_shown': [],
    'setup_ssid': 'Forge-Setup',
    'setup_subnet': '192.168.4',
    'ready_line': 'diag: mark app ready',
    'ip_line': r'net: Connected, IP (\d+\.\d+\.\d+\.\d+)',
    'key_env': 'FORGE_KEY',
    'notes_max_bytes': 20000,
    'repo': '',
    'ota_site': '',
}


def find_root(start=None):
    """The nearest folder at or above `start` (default: this file's folder) that holds forge.json."""
    d = os.path.abspath(start or os.path.dirname(os.path.abspath(__file__)))
    while True:
        if os.path.isfile(os.path.join(d, 'forge.json')):
            return d
        up = os.path.dirname(d)
        if up == d:
            raise FileNotFoundError(f'no forge.json in {start or "this tool"}\'s folder or above it')
        d = up


def load(start=None):
    root = find_root(start)
    with open(os.path.join(root, 'forge.json'), encoding='utf-8') as f:
        cfg = json.load(f)
    out = {**DEFAULTS, **cfg}
    out['screen'] = {**DEFAULTS['screen'], **cfg.get('screen', {})}
    out['_root'] = root
    return out


def path(cfg, *parts):
    """A path under the repository root (forge.json values like build_dir use forward slashes)."""
    return os.path.join(cfg['_root'], *[x for p in parts for x in p.replace('\\', '/').split('/') if x])


def devloop(cfg):
    """<root>/.devloop: the files between the agent, the flash helper and the tools (PROTOCOL.md §1)."""
    d = path(cfg, '.devloop')
    os.makedirs(d, exist_ok=True)
    return d


def https_base(cfg, ip):
    """'https://<ip>' or 'https://<ip>:<port>': forge.json https_port, the firmware's CONFIG_FORGE_HTTPS_PORT (443)."""
    port = int(cfg.get('https_port', 443))
    return f'https://{ip}' if port == 443 else f'https://{ip}:{port}'


def read_cached(cfg, name):
    """.devloop/ip or .devloop/key: '' when absent (both are reset after every flash or install)."""
    try:
        with open(os.path.join(devloop(cfg), name)) as f:
            return f.read().strip()
    except OSError:
        return ''


def write_cached(cfg, name, value):
    with open(os.path.join(devloop(cfg), name), 'w') as f:
        f.write(value or '')
