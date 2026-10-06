/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <string.h>
#include "sb-kernel-hot-wire.h"
static unsigned char wire[32768];
static size_t wire_size;
static unsigned calls;
static int fail_io;
static int memory_transfer(int fd, void *data, size_t bytes, int sending)
{
    (void)fd; calls++;
    if (fail_io) return -1;
    if (sending) {
        assert(bytes <= sizeof(wire));
        memcpy(wire, data, bytes); wire_size=bytes;
    } else {
        if (bytes != wire_size) return -1;
        memcpy(data, wire, bytes);
    }
    return 0;
}
static void roundtrip(uint64_t pages, const uint64_t *values, size_t count, size_t bytes)
{
    uint64_t input[4096],output[4096];
    assert(count <= 4096);
    memcpy(input,values,count*sizeof(*input));
    memset(output,0,sizeof(output));
    assert(!sbk_hot_wire_transfer(0,input,count,pages,1,memory_transfer));
    assert(wire_size==bytes);
    assert(!sbk_hot_wire_transfer(0,output,count,pages,0,memory_transfer));
    assert(!memcmp(values,output,count*sizeof(*output)));
}
int main(void)
{
    const uint64_t small[]={65535,0,42,32768};
    const uint16_t expected[]={65535,0,42,32768};
    roundtrip(65536,small,4,8);
    assert(!memcmp(wire,expected,sizeof(expected)));
    const uint64_t large[]={65536,0,32768,1048575};
    roundtrip(1048576,large,4,16);
    roundtrip(65537,large,3,12);
    uint64_t shuffled[4096];
    for (size_t i=0;i<4096;i++) shuffled[i]=(i*257+123)%1048576;
    roundtrip(1048576,shuffled,4096,16384);
    unsigned before=calls;
    uint64_t bad=2;
    assert(sbk_hot_wire_transfer(0,&bad,1,2,1,memory_transfer)==-ERANGE);
    assert(calls==before);
    const uint16_t malformed=UINT16_MAX;
    memcpy(wire,&malformed,2);wire_size=2;
    assert(sbk_hot_wire_transfer(0,&bad,1,2,0,memory_transfer)==-ERANGE);
    wire_size=1;
    assert(sbk_hot_wire_transfer(0,&bad,1,2,0,memory_transfer)==-EIO);
    bad=1;fail_io=1;
    assert(sbk_hot_wire_transfer(0,&bad,1,2,1,memory_transfer)==-EIO);
    fail_io=0;
    assert(!sbk_hot_wire_transfer(0,NULL,0,1,1,memory_transfer));
    assert(sbk_hot_wire_transfer(0,&bad,UINT64_MAX,2,1,memory_transfer)==-EINVAL);
    assert(sbk_hot_wire_transfer(0,&bad,1,1048577,1,memory_transfer)==-EINVAL);
    assert(sbk_hot_wire_transfer(0,NULL,1,2,0,memory_transfer)==-EINVAL);
    return 0;
}
