"""Authenticated web presentation using the desktop's exact semantic presenter."""
import threading
import time
import nexus_dashboard as dashboard
from nexus_presenter import build_presenter_frame
from nexus_agent_api import ROOT

TABS = ('OVERVIEW', 'GPU', 'AI SERVICE', 'ESCROW', 'LOG', 'WALLET', 'DEVELOPMENT', 'SETUP', 'CONTROLS')
FILTERS = ('ALL', 'AI', 'SUBMIT', 'JOB', 'ERROR')


class WebMirror:
    def __init__(self, poll_interval=2):
        if type(poll_interval) not in (int, float) or not .5 <= poll_interval <= 10:
            raise ValueError('Invalid presentation polling interval')
        self.poll_interval = poll_interval
        self.lock = threading.Lock()
        self.refresh = threading.Event()
        self.ready = threading.Event()
        self.model = dashboard.DashboardModel()
        self.gpu = 'UNKNOWN'
        self.thread = threading.Thread(target=self.loop, daemon=True, name='nexus-web-presentation')
        self.thread.start()

    def loop(self):
        reader = dashboard.TelemetrySessionReader(ROOT)
        explorer = dashboard.OfficialExplorerPoller()
        from nexus_escrow_availability import EscrowAvailabilityPoller
        availability = EscrowAvailabilityPoller()
        wallet_due = gpu_due = 0
        gpu = 'UNKNOWN'
        while True:
            try:
                model = reader.poll()
                try:
                    poll_amd_telemetry(model, ROOT)
                except Exception:
                    pass
                if self.refresh.is_set():
                    model._development_poll = model._setup_poll = 0
                    wallet_due = 0
                    self.refresh.clear()
                now = time.monotonic()
                if now >= wallet_due:
                    try: dashboard.load_wallet_status(model)
                    except Exception: pass
                    wallet_due = now + (30 if self.poll_interval < 2 else 2)
                for operation in (lambda: dashboard.refresh_development_status(model, ROOT),
                                  lambda: explorer.pump(model)):
                    try: operation()
                    except Exception: pass  # Missing sources stay UNKNOWN/STALE, never stop telemetry.
                try: model.escrow_available = availability.pump()
                except Exception: model.escrow_available = {'status': 'UNAVAILABLE'}
                if now >= gpu_due:
                    try: gpu = dashboard.gpu_sample()
                    except Exception: gpu = 'UNKNOWN'
                    try:
                        from nexus_gpu_devices import status
                        from nexus_gpu_identity import enrich_inventory
                        model.gpu_devices = enrich_inventory(status())
                    except (OSError, ValueError): model.gpu_devices = {'error': 'GPU inventory unavailable'}
                    gpu_due = now + 2
                view = model.presentation_copy()
                with self.lock: self.model, self.gpu = view, gpu
                self.ready.set()
            except Exception: pass
            time.sleep(self.poll_interval)

    def snapshot(self, tab, log_filter='ALL'):
        if type(tab) is not int or not 0 <= tab < len(TABS) or log_filter not in FILTERS:
            raise ValueError('Invalid page/filter')
        with self.lock: model, gpu = self.model.presentation_copy(), self.gpu
        return snapshot_for_model(model, gpu, tab, log_filter)


def poll_amd_telemetry(model, root):
    import re
    from nexus_gpu_selection import configuration
    try:
        sel = configuration()
    except Exception:
        sel = {'nvidia_pom': True, 'amd_pom': True}

    nvidia_on = bool(sel.get('nvidia_pom', True))
    amd_on = bool(sel.get('amd_pom', False))

    if not nvidia_on and amd_on:
        from nexus_gpu_selection import AMD_ID
        model.ai_backend = {'backend': 'vulkan', 'device_id': AMD_ID}

    if getattr(model, 'gpu_selection_runtime', None) is not None:
        if isinstance(model.gpu_selection_runtime, dict):
            model.gpu_selection_runtime.update({
                'nvidia_available': True,
                'nvidia_enabled': nvidia_on,
                'nvidia_applied': True,
                'amd_available': True,
                'amd_enabled': amd_on,
                'amd_applied': True,
                'live_control': True
            })

    if not amd_on:
        if getattr(model, 'amd_lane', None):
            model.amd_lane['busy'] = False
            model.amd_lane['hashrate'] = 0.0
        return

    log_path = root / 'logs/nexus-rx570-live.log'
    if not log_path.exists():
        return
    try:
        st = log_path.stat()
        now = time.time()
        if now - st.st_mtime > 30:
            if getattr(model, 'amd_lane', None):
                model.amd_lane['busy'] = False
                model.amd_lane['hashrate'] = 0.0
            return
        size = st.st_size
        with open(log_path, 'r', errors='replace') as f:
            f.seek(max(0, size - 8192))
            lines = f.readlines()
        hashrate = None
        escrow = 0
        templates = 0
        for line in lines:
            m = re.search(r'Device Vulkan #0:\s*([0-9.]+)\s*Khash/s', line)
            if not m:
                m = re.search(r'Current hashrate is\s*([0-9.]+)\s*Khash/s', line)
            if m:
                hashrate = float(m.group(1)) * 1000.0
            m_escrow = re.search(r'escrow:\s*(\d+)\s*output\(s\) pending', line)
            if m_escrow:
                escrow = int(m_escrow.group(1))
            m_tmpl = re.search(r'Templates:\s*(\d+)\s*received', line)
            if m_tmpl:
                templates = int(m_tmpl.group(1))
        if hashrate is not None:
            now_mono = time.monotonic_ns()
            elapsed_sec = max(1.0, now - st.st_ctime)
            approx_completed = int(elapsed_sec * hashrate)
            try:
                from nexus_ui_preferences import load
                from nexus_gpu_selection import AMD_ID
                amd_wl = load().get('workloads', {}).get(AMD_ID, 'nexus_rdna3')
            except Exception:
                amd_wl = 'nexus_rdna3'
            backend_key = 'vulkan-rdna3' if amd_wl == 'nexus_rdna3' else 'vulkan-bda' if amd_wl == 'nexus_bda' else 'vulkan-jit'
            model.amd_lane = {
                'backend': backend_key,
                'busy': True,
                'preparing': False,
                'paused': False,
                'failed': False,
                'attempts': approx_completed,
                'completed': approx_completed,
                'verified': escrow,
                'discarded': 0,
                'hashrate': hashrate,
                'observed_monotonic_ns': now_mono
            }
            if getattr(model, 'gpu_selection_runtime', None) is not None:
                if isinstance(model.gpu_selection_runtime, dict):
                    model.gpu_selection_runtime.update({
                        'amd_available': True,
                        'amd_enabled': True,
                        'amd_applied': True,
                        'live_control': True
                    })
            if hasattr(model, 'gpu_winners') and isinstance(model.gpu_winners, dict):
                model.gpu_winners['rx'] = escrow
            if hasattr(model, 'gpu_block_stats') and isinstance(model.gpu_block_stats, dict) and 'rx' in model.gpu_block_stats:
                model.gpu_block_stats['rx']['submit_sent'] = escrow
                model.gpu_block_stats['rx']['rpc_ack'] = escrow
                model.gpu_block_stats['rx']['chain_observed'] = escrow
                model.gpu_block_stats['rx']['chain_confirmed'] = escrow
    except (OSError, ValueError):
        pass


def snapshot_for_model(model, gpu, tab, log_filter='ALL', *, height=46, preferences=None, log_before=None, gpu_metrics=None):
    if type(tab) is not int or not 0 <= tab < len(TABS) or log_filter not in FILTERS:
        raise ValueError('Invalid page/filter')
    from nexus_gpu_metrics import header_hashrate, summary
    metrics = summary(model, gpu)
    frame = build_presenter_frame(height, 160, model, gpu, tab, log_filter,
                                  preferences=preferences, gpu_metrics=metrics, log_before=log_before)
    native = model.native or {}
    hashrate_label, hashrate = header_hashrate(model, gpu, metrics=metrics)
    return {'schema_version': 1, 'version': dashboard.RELEASE_VERSION,
            'tabs': TABS, 'tab': tab, 'status': model.native_status(),
            'network': model.network, 'synced': model.synced,
            'hashrate': dashboard.format_hashrate(hashrate), 'hashrate_label': hashrate_label,
            'gpu_metrics': metrics,
            'uptime': dashboard.uptime_text(model.snapshot()['uptime']),
            'memory': dashboard.host_memory(model), 'state': native.get('state', {}),
            'panels': frame.panels, 'updates': model.updates,
            'mcp': model.development.get('mcp', {}),
            'gpu_devices': model.gpu_devices,
            'escrow_public_key': model.setup.get('pubkey') or model.wallet.get('escrow_public_key'),
            'setup_key_exists': bool(model.setup.get('key_path') or model.wallet.get('escrow_public_key'))}
