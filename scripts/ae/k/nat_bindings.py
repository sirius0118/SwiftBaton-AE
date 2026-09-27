"""Explicit NAT bindings for the experiment's preserved TCP connections.

Restored TCP may send before a SYN exists on the destination host. Prepare the
exact original/reply tuples before source suspension; ordinary new connections
continue to use the experiment's existing NAT rules. No wildcard deletion.
"""
import ctypes as C
import errno
import socket
import sys
import time
class NatBindings:
    def __init__(self,side,port):
        if side not in ('client','target') or not 1024<=port<=65535:raise ValueError('NAT scope')
        self.side=side;self.port=port;self.created=[]
        l=self.lib=C.CDLL('libnetfilter_conntrack.so.3',use_errno=True)
        for n,a,r in [('nfct_open',[C.c_ubyte,C.c_uint],C.c_void_p),('nfct_close',[C.c_void_p],C.c_int),
          ('nfct_new',[],C.c_void_p),('nfct_destroy',[C.c_void_p],None),('nfct_query',[C.c_void_p,C.c_int,C.c_void_p],C.c_int),
          ('nfct_setobjopt',[C.c_void_p,C.c_uint],C.c_int),('nfct_set_attr_u8',[C.c_void_p,C.c_int,C.c_ubyte],None),
          ('nfct_set_attr_u16',[C.c_void_p,C.c_int,C.c_ushort],None),('nfct_set_attr_u32',[C.c_void_p,C.c_int,C.c_uint],None)]:
            f=getattr(l,n);f.argtypes=a;f.restype=r
        self.handle=l.nfct_open(1,0)
        if not self.handle:raise OSError(C.get_errno(),'NAT conntrack handle')
    def object(self,sport):
        l=self.lib;ct=l.nfct_new()
        if not ct:raise MemoryError('NAT object')
        ip=lambda a:int.from_bytes(socket.inet_aton(a),sys.byteorder)
        l.nfct_set_attr_u8(ct,15,socket.AF_INET);l.nfct_set_attr_u8(ct,17,socket.IPPROTO_TCP)
        l.nfct_set_attr_u32(ct,0,ip('10.0.0.61'))
        l.nfct_set_attr_u32(ct,1,ip('10.0.0.62' if self.side=='client' else '10.0.0.63'))
        l.nfct_set_attr_u16(ct,8,socket.htons(sport));l.nfct_set_attr_u16(ct,9,socket.htons(self.port))
        l.nfct_setobjopt(ct,5)
        l.nfct_set_attr_u8(ct,19,3) # TCP_CONNTRACK_ESTABLISHED
        # Netlink restores the tuple/state, not the old TCP window tracker.
        # A repair-mode window probe can be the first reverse-direction packet;
        # its fresh tracker then misclassifies queued original packets INVALID,
        # so NAT is skipped. Relax *conntrack* window checks on these exact
        # migration tuples only. End-host TCP sequence/window checks stay intact.
        for attr in (33,34,35,36): # FLAGS_ORIG/REPL, MASK_ORIG/REPL
            l.nfct_set_attr_u8(ct,attr,8) # IP_CT_TCP_FLAG_BE_LIBERAL
        l.nfct_set_attr_u32(ct,24,180)
        l.nfct_set_attr_u32(ct,21,ip('10.0.0.63' if self.side=='client' else '172.30.52.3'))
        l.nfct_set_attr_u16(ct,23,socket.htons(self.port if self.side=='client' else 6379))
        return ct
    def remove(self,ports):
        for port in self.checked(ports):
            ct=self.object(port)
            try:
                if self.lib.nfct_query(self.handle,2,ct)<0 and C.get_errno()!=errno.ENOENT:raise OSError(C.get_errno(),'exact NAT delete')
            finally:self.lib.nfct_destroy(ct)
    @staticmethod
    def checked(ports):
        if not isinstance(ports,list) or len(ports)>1024 or len(set(ports))!=len(ports) or any(type(p)!=int or not 1<=p<=65535 for p in ports):raise ValueError('TCP source ports')
        return ports
    def install(self,ports):
        self.checked(ports);started=time.monotonic_ns()
        try:
            for port in ports:
                ct=self.object(port)
                try:
                    # The driver exclusively owns this published destination
                    # port. Remove only the exact tuple's old experiment state.
                    if self.lib.nfct_query(self.handle,2,ct)<0 and C.get_errno()!=errno.ENOENT:raise OSError(C.get_errno(),'stale exact NAT delete')
                    if self.lib.nfct_query(self.handle,0,ct)<0:raise OSError(C.get_errno(),'NAT create')
                    self.created.append(port)
                finally:self.lib.nfct_destroy(ct)
        except BaseException:
            self.remove(self.created);self.created=[];raise
        return {'side':self.side,'ports':ports,'installed':len(ports),'tcp_window_tracking':'per_flow_liberal','duration_ns':time.monotonic_ns()-started}
    def close(self):
        if self.handle:self.lib.nfct_close(self.handle);self.handle=None

if __name__=='__main__':
    import argparse,json
    from pathlib import Path
    p=argparse.ArgumentParser();p.add_argument('role',choices=['cleanup']);p.add_argument('path',type=Path);p.add_argument('name');p.add_argument('side',choices=['client','target']);p.add_argument('port',type=int);a=p.parse_args()
    if a.path.exists():
        data=json.loads(a.path.read_text())
        if data.get('name')!=a.name or data.get('side')!=a.side or data.get('port')!=a.port:raise SystemExit('NAT cleanup ownership mismatch')
        b=NatBindings(a.side,a.port)
        try:b.remove(data['ports'])
        finally:b.close()
