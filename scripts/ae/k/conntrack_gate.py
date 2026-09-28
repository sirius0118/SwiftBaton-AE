"""Delete only live client-to-source Redis conntrack tuples at cutover.

INET_DIAG finds the current TCP sockets. One pre-opened netfilter netlink
socket sends all exact-tuple deletes in a batch and verifies every ACK.
"""
import ctypes as C
import errno
import select
import socket
import struct
import sys
import time
class ConntrackGate:
    CALLBACK=C.CFUNCTYPE(C.c_int,C.c_int,C.c_void_p,C.c_void_p)
    def __init__(self,port,client='10.0.0.61',source='10.0.0.62'):
        if not 1024<=port<=65535:raise ValueError('Redis port')
        self.client=int.from_bytes(socket.inet_aton(client),sys.byteorder)
        self.source=int.from_bytes(socket.inet_aton(source),sys.byteorder);self.port=socket.htons(port)
        self.client_ip=socket.inet_aton(client);self.source_ip=socket.inet_aton(source)
        self.error=None;self.deleted=self.expired=0;self.pump=None;self.ports=[]
        self.dump=None;self.delete=None;self.raw_delete=None
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
        self.dump=l.nfct_open(1,0)
        if not self.dump:self.close();raise OSError(C.get_errno(),'nfct_open dump')
        self.delete=l.nfct_open(1,0)
        if not self.delete:self.close();raise OSError(C.get_errno(),'nfct_open delete')
        self.callback=self.CALLBACK(self.entry)
        if l.nfct_callback_register(self.dump,7,self.callback,None)<0:self.close();raise OSError(C.get_errno(),'nfct_callback_register')
        self.raw_delete=socket.socket(socket.AF_NETLINK,socket.SOCK_RAW,12)
        self.raw_delete.bind((0,0))
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
    def sockets_proc(self):
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
    def _diag_family(self,family):
        # Linux inet_diag_req_v2 asks for TCP sockets in one address family.
        # Parse complete multipart replies; a truncated or error reply fails
        # closed instead of silently omitting connections.
        sock=socket.socket(socket.AF_NETLINK,socket.SOCK_RAW,4)
        try:
            sock.bind((0,0))
            sock.settimeout(2)
            seq=1
            # INET_DIAG_BC_D_EQ rejects other destination ports in the
            # kernel. The final IP checks below still enforce the exact peer.
            bytecode=(struct.pack('=BBH',12,8,12)+
                      struct.pack('=BBH',0,0,socket.ntohs(self.port)))
            request=(struct.pack('=BBBBI',family,socket.IPPROTO_TCP,0,0,0xffffffff)+
                     bytes(48)+self._attribute(1,bytecode))
            sock.send(struct.pack('=IHHII',16+len(request),20,0x301,seq,0)+request)
            ports={}
            while True:
                data,_,flags,_=sock.recvmsg(1<<20)
                if flags & socket.MSG_TRUNC:raise RuntimeError('Truncated socket diagnostic reply')
                offset=0
                while offset+16<=len(data):
                    length,kind,_,reply_seq,_=struct.unpack_from('=IHHII',data,offset)
                    if length<16 or offset+length>len(data) or reply_seq!=seq:
                        raise RuntimeError('Malformed socket diagnostic reply')
                    body=data[offset+16:offset+length]
                    if kind==3:return ports # NLMSG_DONE
                    if kind==2:
                        if len(body)<4:raise RuntimeError('Short socket diagnostic error')
                        raise OSError(-struct.unpack_from('=i',body)[0],'socket diagnostic')
                    if kind!=20 or len(body)<72:raise RuntimeError('Unexpected socket diagnostic message')
                    client=body[8:24];source=body[24:40]
                    if family==socket.AF_INET:
                        match=client[:4]==self.client_ip and source[:4]==self.source_ip
                    else:
                        prefix=b'\0'*10+b'\xff'*2
                        match=(client[:12]==prefix and source[:12]==prefix and
                               client[12:16]==self.client_ip and source[12:16]==self.source_ip)
                    if match and int.from_bytes(body[6:8],'big')==socket.ntohs(self.port):
                        ports[int.from_bytes(body[4:6],'big')]=body[1]
                    offset+=(length+3)&~3
                if offset!=len(data):raise RuntimeError('Trailing socket diagnostic bytes')
        finally:sock.close()
    def sockets(self):
        ports=self._diag_family(socket.AF_INET)
        ipv6=self._diag_family(socket.AF_INET6)
        if set(ports)&set(ipv6):raise RuntimeError('Duplicate TCP source port across address families')
        ports.update(ipv6)
        return ports
    @staticmethod
    def _attribute(kind,payload,nested=False):
        length=4+len(payload)
        return (struct.pack('=HH',length,kind|(0x8000 if nested else 0))+
                payload+b'\0'*((-length)&3))
    def _delete_message(self,sport,seq):
        attr=self._attribute
        ip=attr(1,self.client_ip)+attr(2,self.source_ip)
        proto=(attr(1,bytes((socket.IPPROTO_TCP,)))+
               attr(2,sport.to_bytes(2,'big'))+
               attr(3,socket.ntohs(self.port).to_bytes(2,'big')))
        body=struct.pack('=BBH',socket.AF_INET,0,0)+attr(1,attr(1,ip,True)+attr(2,proto,True),True)
        return struct.pack('=IHHII',16+len(body),(1<<8)|2,5,seq,0)+body
    def _delete_batch(self,ports,pump):
        if not ports:return 0,0
        seq_base=time.monotonic_ns()&0x7fffffff
        pending={seq_base+i for i in range(1,len(ports)+1)}
        payload=b''.join(self._delete_message(port,seq_base+i)
                         for i,port in enumerate(ports,1))
        if self.raw_delete.send(payload)!=len(payload):raise RuntimeError('Short conntrack batch send')
        deleted=expired=0;deadline=time.monotonic()+2
        while pending:
            if time.monotonic()>deadline:raise TimeoutError('Missing conntrack delete ACK')
            if pump:pump()
            if not select.select((self.raw_delete,),(),(),.001)[0]:continue
            data,_,flags,_=self.raw_delete.recvmsg(1<<20)
            if flags&socket.MSG_TRUNC:raise RuntimeError('Truncated conntrack ACK')
            offset=0
            while offset+16<=len(data):
                length,kind,_,seq,_=struct.unpack_from('=IHHII',data,offset)
                if length<20 or offset+length>len(data) or kind!=2 or seq not in pending:
                    raise RuntimeError('Unexpected conntrack ACK')
                error=struct.unpack_from('=i',data,offset+16)[0]
                if error==0:deleted+=1
                elif error==-errno.ENOENT:expired+=1
                else:raise OSError(-error,'exact conntrack delete')
                pending.remove(seq);offset+=(length+3)&~3
            if offset!=len(data):raise RuntimeError('Trailing conntrack ACK bytes')
        return deleted,expired
    def remove(self,pump=None):
        # The OUTPUT fence is already active: new SYNs cannot reach conntrack.
        # Walk actual local TCP sockets (including IPv4-mapped IPv6), not every
        # historical conntrack entry on this host. Only these exact tuples can
        # carry a live client connection across the migration. A nonexistent
        # tuple is valid for a newly queued SYN and remains absent until release.
        started=time.monotonic_ns();ports=self.sockets();scan_ns=time.monotonic_ns()-started
        deleted,expired=self._delete_batch(sorted(ports),pump)
        return {'method':'socket_diag_batched_netlink','deleted':deleted,'already_expired':expired,
                'duration_ns':time.monotonic_ns()-started,'socket_scan_ns':scan_ns,
                'established_ports':sorted(p for p,v in ports.items() if v==1)}
    def close(self):
        if self.raw_delete:self.raw_delete.close();self.raw_delete=None
        if self.dump:self.lib.nfct_close(self.dump);self.dump=None
        if self.delete:self.lib.nfct_close(self.delete);self.delete=None
