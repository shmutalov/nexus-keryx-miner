"""Typed desktop commands. Identity, AI and process gates remain in existing controllers."""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
from nexus_agent_api import ROOT, SOURCE

ACTIONS = frozenset({'preferences', 'workload', 'gpu_select', 'payout', 'key',
    'new_key', 'certificate', 'verify', 'update_check', 'updates_toggle',
    'api_toggle', 'mcp_toggle', 'api_generate', 'copy_api_key', 'copy_public_key',
    'copy_endpoint', 'notifications_toggle', 'notification_status',
    'gpu_capabilities_refresh', 'gpu_mode', 'stop', 'restart', 'shutdown',
    'mining_mode', 'pool_config', 'pool_save', 'pool_delete', 'pool_activate',
    'pool_test_connection', 'pool_reorder', 'force_restart', 'pause'})
CONFIRM = frozenset({'gpu_select', 'workload', 'payout', 'key', 'new_key', 'certificate',
    'verify', 'api_generate', 'stop', 'restart', 'shutdown', 'mining_mode', 'pool_config',
    'pool_save', 'pool_delete', 'pool_activate', 'pool_reorder', 'force_restart'})


def production_source():
    """Dispatch through the active desktop's controller tree, not a candidate launcher."""
    try:
        result = subprocess.run(['systemctl', '--user', 'show', 'nexus-v002-production.service',
            '-p', 'ExecStart', '--value'], capture_output=True, text=True, timeout=2, check=True)
        match = re.search(r'argv\[\]=(.*?) ; ignore_errors=', result.stdout)
        if match:
            for argument in shlex.split(match.group(1)):
                path = Path(argument)
                if path.name == 'nexusmine-next.fish' and path.is_absolute():
                    path = path.resolve(strict=True)
                    info = path.stat()
                    if path.is_relative_to(ROOT.resolve()) and info.st_uid == os.getuid() and not info.st_mode & 0o022:
                        return path.parents[1]
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return SOURCE


def action(name, value, confirmed=False):
    if not isinstance(name, str) or name not in ACTIONS:
        raise ValueError('Unsupported desktop action')
    if name in CONFIRM and confirmed is not True:
        raise ValueError('Explicit confirmation is required')
    if name == 'preferences':
        from nexus_ui_preferences import load, save
        if not isinstance(value, dict) or not value or not set(value) <= {'gpu_view', 'background', 'text', 'font_size', 'glow'}:
            raise ValueError('Invalid presentation settings')
        save(dict(load(), **value))
        return {'message': 'Presentation preference saved; mining workload is unchanged'}
    if name == 'gpu_select':
        if not isinstance(value, dict) or set(value) != {'device', 'enabled'} or not isinstance(value.get('device'), str) or type(value.get('enabled')) is not bool:
            raise ValueError('Invalid GPU configuration')
        from nexus_gpu_selection import CUDA_UUID, AMD_ID, configuration
        from nexus_gpu_devices import STATE, atomic, prepare_state
        dev = value['device']
        if dev not in (CUDA_UUID, AMD_ID):
            raise ValueError(f'Select an authorized GPU: {dev}')
        prepare_state()
        sel = configuration()
        sel['nvidia_pom' if dev == CUDA_UUID else 'amd_pom'] = value['enabled']
        atomic(STATE / 'selection.json', sel)
        import nexus_mining_mode
        restart_res = nexus_mining_mode.restart_miner(force=True, target_device=dev)
        dev_label = 'NVIDIA RTX 3070 Ti' if dev == CUDA_UUID else 'AMD Radeon RX 570'
        state_label = 'ON' if value['enabled'] else 'OFF'
        return {'message': f'{dev_label} set to {state_label} (miner applied)', 'selection': sel, 'restart': restart_res}
    if name == 'workload':
        if not isinstance(value, dict) or set(value) != {'device', 'workload'} or not isinstance(value.get('device'), str) or not isinstance(value.get('workload'), str):
            raise ValueError('Invalid workload configuration')
        from nexus_gpu_selection import CUDA_UUID, AMD_ID
        from nexus_gpu_registry import resolve_workload, WORKLOADS
        from nexus_ui_preferences import load, save
        dev = value['device']
        if dev not in (CUDA_UUID, AMD_ID):
            raise ValueError(f'Select an authorized GPU: {dev}')
        vendor = 'nvidia' if dev == CUDA_UUID else 'amd'
        resolve_workload({'vendor': vendor}, value['workload'])
        prefs = load()
        prefs.setdefault('workloads', {})[dev] = value['workload']
        save(prefs)
        import nexus_mining_mode
        restart_res = nexus_mining_mode.restart_miner(force=True, target_device=dev)
        dev_label = 'NVIDIA RTX 3070 Ti' if dev == CUDA_UUID else 'AMD Radeon RX 570'
        wl_label = WORKLOADS.get(value['workload'], value['workload'])
        return {'message': f'Workload for {dev_label} set to {wl_label} (miner applied)', 'workload': value['workload'], 'restart': restart_res}
    if name in ('payout', 'key', 'new_key', 'certificate', 'verify'):
        from nexus_setup import configure
        if not isinstance(value, str) or len(value) > 1024:
            raise ValueError('Invalid setup value')
        return {'message': configure(name, value)}
    if name in ('stop', 'restart', 'shutdown'):
        from nexus_desktop_commands import controller_command
        subprocess.run(controller_command(name, ROOT, production_source()), check=True,
                       capture_output=True, timeout=5)
        return {'message': 'Miner ' + name + ' requested through the existing controller'}
    if name == 'force_restart':
        import nexus_mining_mode
        res = nexus_mining_mode.restart_miner(force=True)
        return {'message': 'Miner force restarted (all workers and leases cleared)', 'restart': res}
    if name == 'pause':
        import nexus_mining_mode
        return nexus_mining_mode.toggle_miner_pause()
    if name == 'update_check':
        from nexus_updates import check, status
        check(activate=False, inspect_only=True)
        return {'message': 'Version: ' + status().get('status', 'UNKNOWN')}
    if name == 'updates_toggle':
        from nexus_updates import toggle
        return {'message': 'Automatic updates: ' + ('ON' if toggle()['enabled'] else 'OFF')}
    if name in ('api_toggle', 'mcp_toggle'):
        import nexus_dashboard as dashboard
        model = dashboard.DashboardModel()
        dashboard.development_keypress(model, ROOT, ord('T' if name == 'api_toggle' else 'M'))
        return {'message': model.development_notice}
    if name == 'api_generate':
        from nexus_agent_api import ensure_agent_key
        return {'message': ensure_agent_key(rotate=True)}
    if name == 'copy_api_key':
        from nexus_mcp_control import client_token
        return {'message': 'API/MSP key copied to clipboard', 'clipboard': client_token()}
    if name == 'copy_endpoint':
        return {'message': 'MCP endpoint copied to clipboard', 'clipboard': 'http://127.0.0.1:3341/mcp'}
    if name == 'copy_public_key':
        from nexus_setup import draft
        key = draft().get('pubkey')
        if not isinstance(key, str) or not re.fullmatch('[0-9a-f]{64}', key):
            raise ValueError('Escrow public key unavailable')
        return {'message': 'Escrow public key copied to clipboard', 'clipboard': key}
    if name in ('notifications_toggle', 'notification_status'):
        from nexus_notifications import notifications_enabled, set_notifications_enabled
        enabled = notifications_enabled()
        if name == 'notifications_toggle':
            enabled = not enabled
            set_notifications_enabled(enabled)
        return {'message': 'Notifications: ' + ('ON' if enabled else 'OFF'), 'notifications_enabled': enabled}
    if name == 'gpu_capabilities_refresh':
        from nexus_gpu_devices import refresh_capabilities
        return {'message': refresh_capabilities()}
    if name == 'gpu_mode':
        from nexus_gpu_devices import configure_mode
        if value not in ('disabled', 'diagnostics'):
            raise ValueError('Invalid offline GPU mode')
        return {'message': configure_mode(value)}
    if name == 'mining_mode':
        import nexus_mining_mode
        if not isinstance(value, dict) or 'mode' not in value:
            raise ValueError('Invalid mining mode value')
        res = nexus_mining_mode.save({'mode': value['mode']})
        restart_res = nexus_mining_mode.restart_miner(force=True)
        return {'message': f"Mining mode set to {res['mode'].upper()} (miner restarted automatically)", 'mining_mode': res, 'restart': restart_res}
    if name == 'pool_config':
        import nexus_mining_mode
        if not isinstance(value, dict):
            raise ValueError('Invalid pool configuration')
        res = nexus_mining_mode.save(value)
        restart_res = nexus_mining_mode.restart_miner(force=True)
        return {'message': 'Pool configuration saved (miner restarted automatically)', 'mining_mode': res, 'restart': restart_res}
    if name == 'pool_save':
        import nexus_mining_mode
        if not isinstance(value, dict):
            raise ValueError('Invalid pool data')
        res = nexus_mining_mode.save_pool(value)
        pname = value.get('name') or value.get('id') or 'Pool'
        restart_res = nexus_mining_mode.restart_miner(force=True)
        return {'message': f"Pool '{pname}' saved (miner restarted automatically)", 'mining_mode': res, 'restart': restart_res}
    if name == 'pool_delete':
        import nexus_mining_mode
        if not isinstance(value, (dict, str)):
            raise ValueError('Invalid pool id')
        pid = value.get('id', value) if isinstance(value, dict) else value
        res = nexus_mining_mode.delete_pool(str(pid))
        restart_res = nexus_mining_mode.restart_miner(force=True)
        return {'message': f"Pool '{pid}' removed/reset (miner restarted automatically)", 'mining_mode': res, 'restart': restart_res}
    if name == 'pool_activate':
        import nexus_mining_mode
        if not isinstance(value, (dict, str)):
            raise ValueError('Invalid pool id')
        pid = value.get('id', value) if isinstance(value, dict) else value
        res = nexus_mining_mode.activate_pool(str(pid))
        restart_res = nexus_mining_mode.restart_miner(force=True)
        return {'message': f"Pool '{pid}' activated as primary (miner restarted automatically)", 'mining_mode': res, 'restart': restart_res}
    if name == 'pool_test_connection':
        import nexus_mining_mode
        url = value.get('url', value) if isinstance(value, dict) else value
        res = nexus_mining_mode.test_connection(str(url))
        if res.get('ok'):
            msg = f"Connection OK: {res.get('ping_ms', 0):.1f} ms"
        else:
            msg = f"Connection FAILED: {res.get('error', 'Unreachable')}"
        return {'message': msg, 'test_result': res}
    if name == 'pool_reorder':
        import nexus_mining_mode
        order = value.get('order', value) if isinstance(value, dict) else value
        if not isinstance(order, list):
            raise ValueError('Order list required')
        res = nexus_mining_mode.reorder_pools([str(x) for x in order])
        restart_res = nexus_mining_mode.restart_miner(force=True)
        return {'message': 'Pool priority order updated (miner restarted automatically)', 'mining_mode': res, 'restart': restart_res}
    raise ValueError('Unsupported desktop action')
