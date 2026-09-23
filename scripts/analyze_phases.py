#!/usr/bin/env python3
"""Measure within-host stage durations; never label them client downtime."""
import argparse
import json
import re
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('directory', type=Path)
root = p.parse_args().directory
pattern = re.compile(r'SB_PHASE phase=(\S+) pid=(\d+) mono_ns=(\d+) real_ns=(\d+)(?: task=(\d+))?')
events = []
for name in ['dump.log', 'restore.log', 'pageclient.log']:
    for match in pattern.finditer((root / name).read_text(errors='replace')):
        events.append(dict(log=name, phase=match[1], pid=int(match[2]),
                           mono_ns=int(match[3]), real_ns=int(match[4]),
                           task=int(match[5]) if match[5] else None))
pairs = [
    ('restore_stage_ps', 'restore.stage_ps_begin', 'restore.stage_ps_done'),
    ('restore_stage_ps_prune', 'restore.stage_ps_prune_begin', 'restore.stage_ps_prune_done'),
    ('restore_stage_ps_refresh', 'restore.stage_ps_refresh_begin', 'restore.stage_ps_refresh_done'),
    ('restore_stage_validation', 'restore.stage_validate_begin', 'restore.stage_validate_done'),
    ('source_precopy_ps_prune', 'precopy.ps_prune_begin', 'precopy.ps_prune_done'),
    ('source_precopy_ps_refresh', 'precopy.ps_refresh_begin', 'precopy.ps_refresh_done'),
    ('source_precopy_snapshot', 'precopy.snapshot_begin', 'precopy.snapshot_done'),
    ('source_precopy_validation', 'precopy.validate_begin', 'precopy.validate_done'),
    ('source_network_lock', 'dump.is_enter', 'dump.network_locked'),
    ('network_rpc', 'network.rpc_begin', 'network.rpc_done'),
    ('network_enter_ns', 'network.enter_namespace_begin', 'network.enter_namespace_done'),
    ('network_install_rules', 'network.enter_namespace_done', 'network.rules_installed'),
    ('network_exit_ns', 'network.rules_installed', 'network.exit_namespace_done'),
    ('source_pstree', 'dump.network_locked', 'dump.pstree_collected'),
    ('source_task_dump', 'dump.pstree_collected', 'dump.tasks_dumped'),
    ('source_final_images_and_ack', 'dump.tasks_dumped', 'dump.images_flushed'),
    ('restore_ps_mounts', 'restore.ps_begin', 'restore.ps_mounts_ready'),
    ('restore_ps_remaining', 'restore.ps_mounts_ready', 'restore.ps_ready'),
    ('restore_is_shared_state', 'restore.is_images_ready', 'restore.root_task_begin'),
    ('restore_root_task', 'restore.root_task_begin', 'restore.tasks_resumed'),
    ('restore_shared_resources', 'restore.shared_resources_begin', 'restore.shared_resources_done'),
    ('restore_task_fds', 'restore.task_fds_begin', 'restore.task_fds_done'),
    ('restore_task_vmas', 'restore.task_vmas_begin', 'restore.task_vmas_done'),
    ('restore_clone_prepare', 'restore.clone_prepare_begin', 'restore.clone_begin'),
    ('restore_clone', 'restore.clone_begin', 'restore.clone_done'),
    ('restore_clone_cleanup', 'restore.clone_done', 'restore.clone_cleanup_done'),
    ('pageclient_after_images_ready', 'pageclient.images_ready', 'pageclient.servicing_faults'),
    ('pageclient_cache_index', 'pageclient.cache_index_begin', 'pageclient.cache_index_done'),
    ('pageclient_cache_prepare_ps', 'pageclient.cache_prepare_begin', 'pageclient.cache_prepare_done'),
    ('pageclient_ps_dirty_buffer', 'pageclient.ps_dirty_buffer_begin', 'pageclient.ps_dirty_buffer_ready'),
    ('pageclient_dirty_wait', 'pageclient.dirty_wait_begin', 'pageclient.dirty_ready'),
    ('pageclient_dirty_transfer', 'pageclient.dirty_ready', 'pageclient.dirty_received'),
    ('source_ipv4_rules', 'network.iptables_begin', 'network.iptables_done'),
    ('source_ipv6_rules', 'network.ip6tables_begin', 'network.ip6tables_done'),
    ('network_helper_fork', 'network.helper_fork_begin', 'network.helper_fork_done'),
    ('restore_network_helper_fork', 'network.helper_fork_begin', 'network.helper_fork_done'),
    ('restore_ipv4_rules', 'network.iptables_begin', 'network.iptables_done'),
    ('restore_ipv6_rules', 'network.ip6tables_begin', 'network.ip6tables_done'),
    ('restore_network_enter_ns', 'network.enter_namespace_begin', 'network.enter_namespace_done'),
    ('restore_network_exit_ns', 'network.rules_installed', 'network.exit_namespace_done'),
    ('image_rdma_publish_and_ack', 'images.publish_begin', 'images.publish_done'),
    ('task_mappings', 'task.mappings_begin', 'task.mappings_done'),
    ('task_infection', 'task.infect_begin', 'task.infect_done'),
    ('task_files', 'task.files_begin', 'task.files_done'),
    ('fd_options', 'fd.options_begin', 'fd.options_done'),
    ('fd_send', 'fd.options_done', 'fd.send_done'),
    ('source_fd_capacity_ps', 'fd.reserve_ps_begin', 'fd.reserve_ps_done'),
    ('task_pages', 'task.pages_begin', 'task.pages_done'),
    ('task_threads', 'task.threads_begin', 'task.threads_done'),
    ('task_total', 'task.begin', 'task.done'),
]
intervals = []
for label, first, last in pairs:
    # The same network helper runs in dump and restore. Never overwrite the
    # former with the latter; preserve every invocation and its provenance.
    expected_log = ('restore.log' if label.startswith('restore_') else
                    'pageclient.log' if label.startswith('pageclient_') else 'dump.log')
    pending = {}
    occurrence = 0
    for e in events:
        if e['log'] != expected_log:
            continue
        key = (e['pid'], e['task'])
        if e['phase'] == first:
            pending[key] = e
        elif e['phase'] == last and key in pending:
            a = pending.pop(key)
            occurrence += 1
            intervals.append(dict(stage=label, log=expected_log, task=e['task'],
                                  occurrence=occurrence,
                                  milliseconds=(e['mono_ns']-a['mono_ns'])/1e6))
result = dict(events=events, intervals=intervals, note='Within-host CLOCK_MONOTONIC_RAW intervals; not client downtime.')
(root / 'phase-timings.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(intervals, indent=2))
