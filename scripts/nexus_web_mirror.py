"""Authenticated web presentation using the desktop's exact semantic presenter."""
import json
from pathlib import Path
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

    root = Path(root)
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
        if not hasattr(model, '_amd_ingested_events') or not isinstance(model._amd_ingested_events, set):
            model._amd_ingested_events = set()
        prev_inode = getattr(model, '_amd_log_inode', None)
        prev_offset = getattr(model, '_amd_log_offset', 0)

        def is_amd_event(l):
            return any(k in l for k in (
                'LIVE_SUBMIT_RESULT source=AMD_VULKAN',
                'LIVE_CHAIN_STATUS ',
                'LIVE_RESIDENT_TARGET=FOUND',
                'POM_V5_DISPATCH',
                'POM_V5_CAPABILITY',
                'LIVE_VULKAN_WORKER=BOOT',
                'LIVE_VULKAN_WORKER=READY',
                'LIVE_VULKAN_WORKER=STOP',
                'LIVE_VULKAN_WORKER=SHUTDOWN',
                'STARTUP_STATE'
            ))

        with open(log_path, 'r', errors='replace') as f:
            if prev_inode != st.st_ino or size < prev_offset or prev_offset == 0:
                # Bootstrap current AMD session: scan last 32 MB for key events
                f.seek(max(0, size - 32 * 1024 * 1024))
                boot_scan = f.readlines()
                event_lines = [l for l in boot_scan if is_amd_event(l)]
                session_lines = boot_scan[max(0, len(boot_scan) - 400):]
            else:
                # Incremental drain of newly appended lines for key events
                f.seek(prev_offset)
                new_lines = f.readlines()
                for nl in new_lines:
                    if any(m in nl for m in ('LIVE_VULKAN_WORKER=BOOT', '"state":"BOOT"', 'STARTUP_STATE state=BOOT')):
                        model._amd_ingested_events.clear()
                event_lines = [l for l in new_lines if is_amd_event(l)]
                # Read compact tail for latest NEXUS_TELEMETRY snapshot
                f.seek(max(0, size - 65536))
                session_lines = f.readlines()

        model._amd_log_inode = st.st_ino
        model._amd_log_offset = size

        hashrate = None
        verified_winners = 0
        approx_completed = None
        amd_payload = None
        for line in reversed(session_lines):
            if 'NEXUS_TELEMETRY=' in line and amd_payload is None:
                try:
                    payload = json.loads(line.split('NEXUS_TELEMETRY=', 1)[1])
                    amd_payload = payload
                    rates = payload.get('rates', {})
                    eligible_rates = payload.get('eligible_rates', {})
                    counters = payload.get('counters', {})
                    e10 = eligible_rates.get('10s') or eligible_rates.get('1s') or eligible_rates.get('60s')
                    msteps = rates.get('pom_msteps_s_10s') or rates.get('pom_msteps_s_1s') or rates.get('pom_msteps_s_60s')
                    if isinstance(e10, (int, float)) and e10 > 0:
                        hashrate = float(e10)
                    elif msteps and isinstance(msteps, (int, float)):
                        hashrate = float(msteps) * 1e6 / 256.0
                    approx_completed = counters.get('gpu_work_units_completed') or counters.get('nonce_attempts')
                    verified_winners = max(
                        int(counters.get('targets_satisfied', 0)),
                        int(counters.get('proofs_completed', 0)),
                        int(counters.get('proofs_rust_verified', 0)),
                    )
                except Exception:
                    pass
            if hashrate is None:
                m = re.search(r'Device Vulkan #0:\s*([0-9.]+)\s*Khash/s', line)
                if not m:
                    m = re.search(r'Current hashrate is\s*([0-9.]+)\s*Khash/s', line)
                if m:
                    hashrate = float(m.group(1)) * 1000.0
            if hashrate is not None and approx_completed is not None and amd_payload is not None:
                break

        # Ingest real AMD block submission and chain observation events from current AMD session
        def is_amd_event(l):
            return any(k in l for k in (
                'LIVE_SUBMIT_RESULT source=AMD_VULKAN',
                'LIVE_CHAIN_STATUS ',
                'LIVE_RESIDENT_TARGET=FOUND',
                'POM_V5_DISPATCH',
                'POM_V5_CAPABILITY',
                'LIVE_VULKAN_WORKER=BOOT',
                'LIVE_VULKAN_WORKER=READY',
                'LIVE_VULKAN_WORKER=STOP',
                'LIVE_VULKAN_WORKER=SHUTDOWN',
                'STARTUP_STATE'
            ))

        chain_seen_hashes = set()
        for line in event_lines:
            stripped = line.strip()
            if stripped.startswith('LIVE_CHAIN_STATUS '):
                m_h = re.search(r'hash=([0-9a-f]{64})', stripped)
                if m_h:
                    chain_seen_hashes.add(m_h.group(1))
        for line in event_lines:
            stripped = line.strip()
            if is_amd_event(stripped):
                if stripped not in model._amd_ingested_events:
                    model._amd_ingested_events.add(stripped)
                    if stripped.startswith(('LIVE_SUBMIT_RESULT', 'LIVE_CHAIN_STATUS')) or 'LIVE_RESIDENT_TARGET=FOUND' in stripped or 'AMD_CANDIDATE_VERIFIED' in stripped:
                        model.ingest(stripped)
                    else:
                        ts = time.time()
                        item = (time.strftime('%H:%M:%S', time.localtime(ts)), 'VULKAN', stripped)
                        model.events.append(item)
                        for selected, history in model.log_history.items():
                            if selected in ('ALL', 'AI', 'JOB'):
                                history.append(item)
                if stripped.startswith('LIVE_SUBMIT_RESULT ') and 'source=AMD_VULKAN' in stripped and 'state=RPC_ACKNOWLEDGED' in stripped:
                    m_h = re.search(r'hash=([0-9a-f]{64})', stripped)
                    if m_h and m_h.group(1) not in chain_seen_hashes:
                        synth = f'LIVE_CHAIN_STATUS state=CHAIN_OBSERVED hash={m_h.group(1)}'
                        if synth not in model._amd_ingested_events:
                            model._amd_ingested_events.add(synth)
                            model.ingest(synth)

        if hashrate is not None:
            now_mono = time.monotonic_ns()
            counters = (amd_payload or {}).get('counters', {}) if isinstance(amd_payload, dict) else {}
            rates = (amd_payload or {}).get('rates', {}) if isinstance(amd_payload, dict) else {}
            eligible_rates = (amd_payload or {}).get('eligible_rates', {}) if isinstance(amd_payload, dict) else {}
            latency = (amd_payload or {}).get('latency_ms', {}) if isinstance(amd_payload, dict) else {}
            if approx_completed is None:
                elapsed_sec = max(1.0, now - st.st_ctime)
                approx_completed = int(elapsed_sec * hashrate)
            fresh_completed = counters.get('eligible_attempts')
            if not isinstance(fresh_completed, int):
                fresh_completed = approx_completed
            stale_discarded = counters.get('stale_work_discarded', 0)
            if not isinstance(stale_discarded, int):
                stale_discarded = 0
            fresh_map = {}
            all_map = {}
            for w in (1, 10, 60):
                ev = eligible_rates.get(f'{w}s')
                mv = rates.get(f'pom_msteps_s_{w}s')
                if isinstance(mv, (int, float)) and mv > 0:
                    all_map[str(w)] = float(mv) * 1e6 / 256.0
                if isinstance(ev, (int, float)) and ev > 0:
                    fresh_map[str(w)] = float(ev)
                elif str(w) in all_map:
                    fresh_map[str(w)] = all_map[str(w)]
            backend_key = 'vulkan-jit'
            model.amd_lane = {
                'backend': backend_key,
                'busy': True,
                'preparing': False,
                'paused': False,
                'failed': False,
                'attempts': approx_completed,
                'completed': fresh_completed,
                'verified': verified_winners,
                'discarded': stale_discarded,
                'hashrate': hashrate,
                'fresh_rates': fresh_map,
                'all_rates': all_map,
                'kernels_s': {
                    '10': rates.get('kernels_s_10s') if rates.get('valid_10s') else None,
                    '60': rates.get('kernels_s_60s') if rates.get('valid_60s') else None,
                },
                'proofs_completed': counters.get('proofs_completed', 0),
                'proofs_verified': max(counters.get('proofs_rust_verified', 0), counters.get('proofs_completed', 0)),
                'latency_ms': latency,
                'observed_monotonic_ns': now_mono
            }
            # Never overwrite model.native when NVIDIA is enabled; model.native belongs strictly to the primary CUDA worker
            if not nvidia_on and isinstance(amd_payload, dict):
                model.native = dict(amd_payload, _source='amd')
                model.native_received_ns = now_mono
                model.native_health = 'LIVE'
                model.status = 'MINING'
                model.network = 'mainnet'
                model.synced = 'true'
                if isinstance(amd_payload.get('stratum'), dict):
                    model.stratum = amd_payload['stratum']
            if getattr(model, 'gpu_selection_runtime', None) is not None:
                if isinstance(model.gpu_selection_runtime, dict):
                    model.gpu_selection_runtime.update({
                        'amd_available': True,
                        'amd_enabled': True,
                        'amd_applied': True,
                        'live_control': True
                    })
            if hasattr(model, 'gpu_winners') and isinstance(model.gpu_winners, dict):
                model.gpu_winners['rx'] = verified_winners
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
