#!/usr/bin/env python3
"""Two-sided AE cutover with a client OUTPUT packet queue, before conntrack."""
import json
from pathlib import Path
import selectors
import socket
import time
from packet_gate import PacketGate
from conntrack_gate import ConntrackGate
from nat_bindings import NatBindings

def listen(config,out):
    from fast_cutover import write_json,send
    gate=PacketGate(config['name'],config['port'])
    conntrack=ConntrackGate(config['port'])
    bindings=NatBindings('client',config['port'])
    server=socket.socket(); selector=selectors.DefaultSelector(); peers={}; pending={}
    result={'ok':False,'buffered':True,'nat_installed':False,'nft_nat_installed':False}
    ack_at=None; awaiting_nat=False; closed=False; isolated=False; nat_prepared=False; success=False
    try:
        server.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
        server.bind(('10.0.0.61',0));server.listen(2);server.setblocking(False)
        selector.register(server,selectors.EVENT_READ,'server')
        selector.register(gate.sock,selectors.EVENT_READ,'queue')
        write_json(out/'cutover.ready',{'port':server.getsockname()[1]})
        deadline=time.monotonic()+180
        while not success:
            if time.monotonic()>deadline:raise TimeoutError('Buffered cutover peer/marker timed out')
            for key,_ in selector.select(.001 if ack_at else .01):
                if key.data=='server':
                    c,address=server.accept();c.setblocking(False)
                    if address[0] not in ('10.0.0.62','10.0.0.63'):c.close();raise ValueError('Unexpected cutover peer')
                    pending[c]={'address':address[0],'buffer':b'','role':None}
                    selector.register(c,selectors.EVENT_READ,'control')
                elif key.data=='queue':gate.drain()
                else:
                    c=key.fileobj;v=pending[c];data=c.recv(4096)
                    if not data:raise RuntimeError('Cutover peer disconnected')
                    v['buffer']+=data
                    if len(v['buffer'])>4096:raise ValueError('Oversized control frame')
                    if b'\n' not in v['buffer']:continue
                    line,v['buffer']=v['buffer'].split(b'\n',1)
                    if v['buffer']:raise ValueError('Unsolicited pipelined control messages')
                    frame=json.loads(line)
                    if v['role'] is None:
                        role=frame.get('role')
                        if (frame != {'op':'arm','role':role,'token':config['token']} or
                            role not in ('source','target') or role in peers or
                            v['address'] != {'source':'10.0.0.62','target':'10.0.0.63'}[role]):
                            raise ValueError('Invalid cutover authentication/role')
                        peers[role]=c;v['role']=role;c.sendall(b'{"status":"armed"}\n')
                    elif frame=={'op':'close'} and v['role']=='source':
                        if closed or awaiting_nat or ack_at or 'target' not in peers:raise RuntimeError('Cutover not armed on both ends')
                        result['received_time_ns']=time.time_ns();result.update(gate.activate());deadline=time.monotonic()+15
                        result['nft_nat_installed']=True
                        # Drain packets already in flight before source network
                        # isolation. NAT preparation can then overlap image dump.
                        ack_at=time.monotonic()+.002
                    elif frame=={'op':'isolated'} and v['role']=='source':
                        if not closed or isolated:raise RuntimeError('Unexpected source isolation')
                        isolated=True;result['source_isolated_received_ns']=time.time_ns()
                        result['conntrack']=conntrack.remove(gate.drain)
                        result['conntrack_done_ns']=time.time_ns()
                        ports=result['conntrack']['established_ports']
                        result['client_nat']=bindings.install(ports)
                        write_json(out/'cutover-nat-client.json',{'name':config['name'],'port':config['port'],'side':'client','ports':ports})
                        peers['target'].sendall(json.dumps({'op':'nat','ports':ports}).encode()+b'\n')
                        awaiting_nat=True
                    elif frame.get('op')=='nat_ready' and v['role']=='target':
                        if not awaiting_nat or set(frame)!={'op','result'}:raise RuntimeError('Unexpected NAT acknowledgement')
                        stats=frame['result']
                        if not isinstance(stats,dict) or stats.get('ports')!=result['conntrack']['established_ports'] or stats.get('installed')!=len(stats['ports']):raise RuntimeError('Target NAT count/identity mismatch')
                        result['target_nat']=stats;awaiting_nat=False;nat_prepared=True
                    elif frame=={'op':'release'} and v['role']=='target':
                        if not closed or not nat_prepared:raise RuntimeError('Restore readiness before isolation/NAT ACK')
                        result.update(gate.release());result.update(ok=True,completed_time_ns=time.time_ns())
                        result['gate_hold_ms']=(result['release_begin_ns']-result['activate_ack_ns'])/1e6
                        result['gate_interval_upper_ms']=(result['release_done_ns']-result['activate_begin_ns'])/1e6
                        write_json(out/'cutover-client.json',result)
                        for peer in peers.values():peer.sendall(json.dumps(result).encode()+b'\n')
                        success=True;break
                    else:raise ValueError('Invalid cutover transition')
            if ack_at and time.monotonic()>=ack_at:
                gate.drain();result.update(gate.drops());result['gate_closed_ack_ns']=time.time_ns()
                peers['source'].sendall(b'{"status":"closed"}\n');closed=True;ack_at=None
        return result
    except BaseException as e:
        result['error']=str(e);write_json(out/'cutover-client.json',result);raise
    finally:
        conntrack.close()
        if not success:bindings.remove(bindings.created)
        bindings.close()
        selector.close();server.close()
        for c in pending:c.close()
        gate.close(success)

def wait_marker(path,timeout=180):
    deadline=time.monotonic()+timeout
    while not path.is_file():
        if time.monotonic()>deadline:raise TimeoutError('No marker: '+str(path))
        time.sleep(.0002)

def peer(config,out,role):
    from fast_cutover import receive,send,write_json
    images=Path(config['stop_file']).parent
    target_nat=NatBindings('target',config['port']) if role=='target' else None
    with socket.create_connection(('10.0.0.61',config['listen_port']),timeout=10) as c:
        c.settimeout(180);c.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        with c.makefile('rwb') as stream:
            send(stream,{'op':'arm','role':role,'token':config['token']})
            if receive(stream)!={'status':'armed'}:raise RuntimeError('Gate did not arm')
            if role=='source':
                write_json(out/'cutover.connected',{'time_ns':time.time_ns()})
                wait_marker(images/'sb-gate-request')
                observed=time.time_ns();start=time.monotonic_ns();send(stream,{'op':'close'})
                if receive(stream)!={'status':'closed'}:raise RuntimeError('Gate did not close')
                write_json(images/'sb-gate-closed',{'closed_time_ns':time.time_ns()})
                wait_marker(Path(config['stop_file']))
                send(stream,{'op':'isolated'})
                result=receive(stream);result['source_observed_time_ns']=observed;result['source_ack_time_ns']=time.time_ns()
                result['trigger_to_ack_ms']=(time.monotonic_ns()-start)/1e6
                write_json(out/'cutover.result.json',result)
            else:
                write_json(out/'cutover.target-connected',{'time_ns':time.time_ns()})
                request=receive(stream)
                if set(request)!={'op','ports'} or request['op']!='nat':raise ValueError('Expected exact NAT preparation')
                stats=target_nat.install(request['ports'])
                write_json(out/'cutover-nat-target.json',{'name':config['name'],'port':config['port'],'side':'target','ports':request['ports']})
                target_nat.close()
                write_json(images/'sb-nat-ready',{'time_ns':time.time_ns()})
                send(stream,{'op':'nat_ready','result':stats})
                wait_marker(images/'sb-tasks-resumed')
                send(stream,{'op':'release'});result=receive(stream)
                write_json(out/'cutover-target.json',result)
            if not result['ok']:raise RuntimeError(result.get('error','Cutover failed'))
