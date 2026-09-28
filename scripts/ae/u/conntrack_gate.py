"""Delete only this client's IPv4/TCP flows to the owned source Redis port.

Uses stable libnetfilter_conntrack ABI from the distribution headers. Two PS
opened handles keep dump callbacks and exact-tuple deletions independent; no
shell/process creation lies between closing the gate and freezing the source.
"""
import ctypes as C
import errno
import socket
import sys
import time
class ConntrackGate:
    CALLBACK=C.CFUNCTYPE(C.c_int,C.c_int,C.c_void_p,C.c_void_p)
    def __init__(self,port,client='10.0.0.61',source='10.0.0.62'):
        if not 1024<=port<=65535:raise ValueError('Redis port')
        self.client=int.from_bytes(socket.inet_aton(client),sys.byteorder)
        self.source=int.from_bytes(socket.inet_aton(source),sys.byteorder);self.port=socket.htons(port)
        self.error=None;self.deleted=self.expired=0;self.pump=None;self.ports=[]
        l=self.lib=C.CDLL('libnetfilter_conntrack.so.3',use_errno=True)
        specs={'nfct_open':([C.c_ubyte,C.c_uint],C.c_void_p),
               'nfct_close':([C.c_void_p],C.c_int),
               'nfct_new':([],C.c_void_p),'nfct_destroy':([C.c_void_p],None),
               'nfct_set_attr_u8':([C.c_void_p,C.c_int,C.c_ubyte],None),
               'nfct_set_attr_u16':([C.c_void_p,C.c_int,C.c_ushort],None),
               'nfct_set_attr_u32':([C.c_void_p,C.c_int,C.c_uint],None),
               'nfct_query':([C.c_void_p,C.c_int,C.c_void_p],C.c_int),
               'nfct_callback_register':([C.c_void_p,C.c_int,self.CALLBACK,C.c_void_p],C.c_int),
               'nfct_get_attr_u8':([C.c_void_p,C.c_int],C.c_ubyte),
               'nfct_get_attr_u16':([C.c_void_p,C.c_int],C.c_ushort),
               'nfct_get_attr_u32':([C.c_void_p,C.c_int],C.c_uint)}
        for n,(a,r) in specs.items():f=getattr(l,n);f.argtypes=a;f.restype=r
        self.dump=l.nfct_open(1,0);self.delete=l.nfct_open(1,0)
        if not self.dump or not self.delete:self.close();raise OSError(C.get_errno(),'nfct_open')
        self.callback=self.CALLBACK(self.entry)
        if l.nfct_callback_register(self.dump,7,self.callback,None)<0:self.close();raise OSError(C.get_errno(),'nfct_callback_register')
    def entry(self,event,ct,opaque):
        try:
            l=self.lib
            if (l.nfct_get_attr_u8(ct,15)!=socket.AF_INET or l.nfct_get_attr_u8(ct,17)!=socket.IPPROTO_TCP or
                l.nfct_get_attr_u32(ct,0)!=self.client or l.nfct_get_attr_u32(ct,1)!=self.source or
                l.nfct_get_attr_u16(ct,9)!=self.port):return 1
            if l.nfct_get_attr_u8(ct,19)==3:self.ports.append(socket.ntohs(l.nfct_get_attr_u16(ct,8)))
            if self.pump:self.pump()
            if l.nfct_query(self.delete,2,ct)<0:
                e=C.get_errno()
                if e!=errno.ENOENT:raise OSError(e,'exact conntrack delete')
                self.expired+=1
            else:self.deleted+=1
            return 1
        except BaseException as e:self.error=e;return -1
    def remove_dump(self,pump=None):
        self.error=None;self.deleted=self.expired=0;self.pump=pump;self.ports=[]
        family=C.c_ubyte(socket.AF_INET);started=time.monotonic_ns()
        rc=self.lib.nfct_query(self.dump,5,C.byref(family))
        if self.error:raise self.error
        if rc<0:raise OSError(C.get_errno(),'conntrack dump')
        return {'method':'libnetfilter_conntrack','deleted':self.deleted,'already_expired':self.expired,'duration_ns':time.monotonic_ns()-started,'established_ports':sorted(set(self.ports))}
    @staticmethod
    def ipv4_address(text):
        if len(text)==8:return int(text,16)
        if len(text)!=32:raise ValueError('Malformed proc TCP address')
        raw=b''.join(int(text[i:i+8],16).to_bytes(4,sys.byteorder) for i in range(0,32,8))
        if raw[:12]!=b'\0'*10+b'\xff\xff':return None
        return int.from_bytes(raw[12:],sys.byteorder)
    def sockets(self):
        ports={}
        for path in ('/proc/net/tcp','/proc/net/tcp6'):
            try:f=open(path)
            except FileNotFoundError:
                if path.endswith('tcp6'):continue
                raise
            with f:
                next(f)
                for line in f:
                    fields=line.split()
                    local,lport=fields[1].split(':');remote,rport=fields[2].split(':')
                    if (self.ipv4_address(local)!=self.client or self.ipv4_address(remote)!=self.source or
                        int(rport,16)!=socket.ntohs(self.port)):continue
                    ports[int(lport,16)]=int(fields[3],16)
        return ports
    def remove(self,pump=None):
        # The OUTPUT fence is already active: new SYNs cannot reach conntrack.
        # Walk actual local TCP sockets (including IPv4-mapped IPv6), not every
        # historical conntrack entry on this host. Only these exact tuples can
        # carry a live client connection across the migration. A nonexistent
        # tuple is valid for a newly queued SYN and remains absent until release.
        started=time.monotonic_ns();ports=self.sockets();deleted=expired=0
        for sport in sorted(ports):
            if pump:pump()
            l=self.lib;ct=l.nfct_new()
            if not ct:raise MemoryError('Exact conntrack tuple')
            try:
                l.nfct_set_attr_u8(ct,15,socket.AF_INET);l.nfct_set_attr_u8(ct,17,socket.IPPROTO_TCP)
                l.nfct_set_attr_u32(ct,0,self.client);l.nfct_set_attr_u32(ct,1,self.source)
                l.nfct_set_attr_u16(ct,8,socket.htons(sport));l.nfct_set_attr_u16(ct,9,self.port)
                if l.nfct_query(self.delete,2,ct)<0:
                    if C.get_errno()!=errno.ENOENT:raise OSError(C.get_errno(),'live socket conntrack delete')
                    expired+=1
                else:deleted+=1
            finally:l.nfct_destroy(ct)
        return {'method':'live_tcp_exact_tuples','deleted':deleted,'already_expired':expired,
                'duration_ns':time.monotonic_ns()-started,'established_ports':sorted(p for p,v in ports.items() if v==1)}
    def close(self):
        if self.dump:self.lib.nfct_close(self.dump);self.dump=None
        if self.delete:self.lib.nfct_close(self.delete);self.delete=None
