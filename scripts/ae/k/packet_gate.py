#!/usr/bin/env python3
"""An experiment-scoped OUTPUT queue. Packets stay in the kernel until release.

Install before conntrack/NAT so held packets use the destination selected at
release. No queue bypass or fail-open flag: errors fail the experiment closed.
The listener must keep draining netlink while packets are held. NAT survives
successful release until cleanup; other interfaces/ports/tables are untouched.
"""
import ctypes as C
import errno
import os
import re
import socket
import time

class Nft:
    def __init__(self):
        self.lib = l = C.CDLL('libnftables.so.1', use_errno=True)
        l.nft_ctx_new.argtypes = [C.c_uint]; l.nft_ctx_new.restype = C.c_void_p
        l.nft_ctx_free.argtypes = [C.c_void_p]
        l.nft_ctx_buffer_error.argtypes = [C.c_void_p]
        l.nft_ctx_get_error_buffer.argtypes = [C.c_void_p]; l.nft_ctx_get_error_buffer.restype = C.c_char_p
        l.nft_run_cmd_from_buffer.argtypes = [C.c_void_p, C.c_char_p]; l.nft_run_cmd_from_buffer.restype = C.c_int
        self.ctx = l.nft_ctx_new(0)
        if not self.ctx: raise MemoryError('nft context')
        l.nft_ctx_buffer_error(self.ctx)
    def run(self, text):
        if self.lib.nft_run_cmd_from_buffer(self.ctx, text.encode()):
            raise RuntimeError(self.lib.nft_ctx_get_error_buffer(self.ctx).decode())
    def close(self):
        if self.ctx: self.lib.nft_ctx_free(self.ctx); self.ctx = None

class PacketGate:
    CALLBACK = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p, C.c_void_p, C.c_void_p)
    def __init__(self, name, port, source='10.0.0.62', target='10.0.0.63', queue=53190):
        if not re.fullmatch(r'sb_ae_[a-zA-Z0-9_]{1,48}', name): raise ValueError('gate owner')
        if not 1024 <= port <= 65535 or not 1 <= queue <= 65535: raise ValueError('port/queue')
        for address in (source, target): socket.inet_pton(socket.AF_INET, address)
        self.table='gate_'+name; self.port=port; self.source=source; self.target=target; self.number=queue
        self.ids=[]; self.hold=True; self.error=None; self.received=0; self.released=0
        self.maximum=0; self.closed=False; self.active=False; self.ready=False
        self.handle=self.qhandle=self.sock=None; self.nft=None; self.success=False
        l=self.lib=C.CDLL('libnetfilter_queue.so.1', use_errno=True)
        specs={
          'nfq_open':([],C.c_void_p), 'nfq_close':([C.c_void_p],C.c_int),
          'nfq_bind_pf':([C.c_void_p,C.c_ushort],C.c_int),
          'nfq_create_queue':([C.c_void_p,C.c_ushort,self.CALLBACK,C.c_void_p],C.c_void_p),
          'nfq_destroy_queue':([C.c_void_p],C.c_int), 'nfq_fd':([C.c_void_p],C.c_int),
          'nfq_set_mode':([C.c_void_p,C.c_ubyte,C.c_uint],C.c_int),
          'nfq_set_queue_maxlen':([C.c_void_p,C.c_uint],C.c_int),
          'nfq_set_queue_flags':([C.c_void_p,C.c_uint,C.c_uint],C.c_int),
          'nfq_get_msg_packet_hdr':([C.c_void_p],C.c_void_p),
          'nfq_handle_packet':([C.c_void_p,C.c_void_p,C.c_int],C.c_int),
          'nfq_set_verdict':([C.c_void_p,C.c_uint,C.c_uint,C.c_uint,C.c_void_p],C.c_int)}
        for fn,(args,result) in specs.items():
            f=getattr(l,fn);f.argtypes=args;f.restype=result
        try:
            self.handle=l.nfq_open()
            if not self.handle: raise OSError(C.get_errno(),'nfq_open')
            # Do not unbind AF_INET globally: other applications may own queues.
            self.check(l.nfq_bind_pf(self.handle,socket.AF_INET),'bind family')
            self.callback=self.CALLBACK(self.packet)
            self.qhandle=l.nfq_create_queue(self.handle,queue,self.callback,None)
            if not self.qhandle: raise OSError(C.get_errno(),'queue already owned/unavailable')
            self.check(l.nfq_set_mode(self.qhandle,1,0),'copy metadata')
            self.check(l.nfq_set_queue_maxlen(self.qhandle,16384),'queue capacity')
            self.check(l.nfq_set_queue_flags(self.qhandle,4,4),'GSO metadata')
            self.sock=socket.socket(fileno=os.dup(l.nfq_fd(self.handle)))
            self.sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,4*1024*1024)
            self.sock.setblocking(False)
            self.nft=Nft()
            self.nft.run(f'''add table ip {self.table}
add chain ip {self.table} gate {{ type filter hook output priority -310; policy accept; }}
add chain ip {self.table} route {{ type nat hook output priority -110; policy accept; }}
add set ip {self.table} ports {{ type inet_service; }}
add set ip {self.table} held {{ type inet_service; }}
add rule ip {self.table} gate ip daddr {source} tcp dport @held queue num {queue}
add rule ip {self.table} route ip daddr {source} tcp dport @ports dnat to {target}:{port}
''')
            self.ready=True
        except BaseException:
            self.close(False); raise
    @staticmethod
    def check(rc, action):
        if rc < 0: raise OSError(C.get_errno(), action)
    def verdict(self, ident, value):
        self.check(self.lib.nfq_set_verdict(self.qhandle,ident,value,0,None),'packet verdict')
    def packet(self, queue, msg, data, opaque):
        try:
            hdr=self.lib.nfq_get_msg_packet_hdr(data)
            if not hdr: raise RuntimeError('Missing packet ID')
            ident=int.from_bytes(C.string_at(hdr,4),'big'); self.received+=1
            if self.hold:
                self.ids.append(ident); self.maximum=max(self.maximum,len(self.ids))
            else:
                self.verdict(ident,1); self.released+=1
        except BaseException as e: self.error=e
        return 0
    def drain(self):
        while True:
            try: data,_,flags,_=self.sock.recvmsg(65536)
            except BlockingIOError: break
            if flags & socket.MSG_TRUNC: raise RuntimeError('Truncated netlink queue message')
            self.check(self.lib.nfq_handle_packet(self.handle,C.c_char_p(data),len(data)),'netlink packet')
            if self.error: raise self.error
    def activate(self):
        if self.active or not self.ready: raise RuntimeError('Gate state')
        before=time.time_ns()
        self.nft.run(f'''add element ip {self.table} held {{ {self.port} }}
add element ip {self.table} ports {{ {self.port} }}
''')
        self.active=True; self.activated_ns=time.time_ns()
        return {'activate_begin_ns':before,'activate_ack_ns':self.activated_ns}
    def drops(self):
        with open('/proc/net/netfilter/nfnetlink_queue') as f:
            rows=[x.split() for x in f if x.split() and int(x.split()[0])==self.number]
        if len(rows)!=1 or len(rows[0])<8: raise RuntimeError('Queue stats unavailable')
        row=rows[0]
        result={'queue_drops':int(row[5]),'netlink_drops':int(row[6])}
        if any(result.values()): raise RuntimeError('Queued packets lost: '+str(result))
        return result
    def release(self):
        if not self.active or not self.hold: raise RuntimeError('Gate is not closed')
        self.drain(); self.drops(); self.hold=False
        started=time.time_ns()
        # Accept before removing the hook. Packets arriving during the release
        # are drained and accepted too; the queue is closed only after removal.
        for ident in self.ids: self.verdict(ident,1); self.released+=1
        self.ids.clear()
        self.nft.run(f'flush set ip {self.table} held\n')
        self.drain(); self.active=False; self.success=True
        return {'release_begin_ns':started,'release_done_ns':time.time_ns(),
                'queued_packets':self.received,'maximum_held':self.maximum,'released_packets':self.released,**self.drops()}
    def close(self, keep_nat=False):
        if self.closed:return
        self.closed=True
        try:
            if self.nft:
                try:
                    if keep_nat and self.success:self.nft.run(f'flush set ip {self.table} held\n')
                    elif self.ready:self.nft.run(f'delete table ip {self.table}\n')
                finally:self.nft.close()
        finally:
            if self.sock:self.sock.close()
            if self.qhandle:self.lib.nfq_destroy_queue(self.qhandle)
            if self.handle:self.lib.nfq_close(self.handle)

def cleanup(name):
    if not re.fullmatch(r'sb_ae_[a-zA-Z0-9_]{1,48}',name):raise ValueError('gate owner')
    nft=Nft()
    try:
        # Only absence is ignorable. Permission/protocol errors remain errors.
        try:nft.run(f'list table ip gate_{name}\n')
        except RuntimeError as e:
            if 'No such file or directory' not in str(e):raise
        else:nft.run(f'delete table ip gate_{name}\n')
    finally:nft.close()

if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('role',choices=['cleanup']);p.add_argument('name');a=p.parse_args();cleanup(a.name)
