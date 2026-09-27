// SPDX-License-Identifier: GPL-2.0
/* Isolated-VM-only ownership/rollback tests of the exported MM bridge. */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/swiftbaton_pte.h>
struct request { u64 address; u32 pages, mode; };
static atomic64_t released=ATOMIC64_INIT(0), unexpected=ATOMIC64_INIT(0);
static struct page *no_fault(void *cookie, struct vm_area_struct *vma, unsigned long address, unsigned int lane, bool user) {
 atomic64_inc(&unexpected);return ERR_PTR(-EIO);
}
static void token_release(void *cookie){atomic64_inc(&released);}
static const struct sbk_pte_provider provider={.owner=THIS_MODULE,.get_page=no_fault,.release=token_release};
static long test_ioctl(struct file *file,unsigned int cmd,unsigned long arg) {
 struct request r;unsigned long *ids;unsigned long saved=0;unsigned i,created=0;int ret;
 if(cmd==_IOR('Z',0x52,u64[2])){u64 stats[2]={atomic64_read(&released),atomic64_read(&unexpected)};return copy_to_user((void __user *)arg,stats,sizeof(stats))?-EFAULT:0;}
 if(cmd!=_IOW('Z',0x51,struct request) || copy_from_user(&r,(void __user *)arg,sizeof(r)))return -EINVAL;
 if(!r.pages || r.pages>2048 || r.mode>3)return -EINVAL;
 ids=kcalloc(r.pages,sizeof(*ids),GFP_KERNEL);if(!ids)return -ENOMEM;
 for(i=0;i<r.pages;i++){ret=sbk_pte_token_create(&provider,NULL,&ids[i]);if(ret)goto out;created++;}
 /* mode1 deliberately repeats a live token: insertion of the first PTE
  * succeeds; the second sees its anchor, forcing partial-install rollback. */
 if(r.mode==1){if(r.pages<2){ret=-EINVAL;goto out;}saved=ids[1];ids[1]=ids[0];}
 if(r.mode==2){saved=ids[r.pages/2];ids[r.pages/2]=0;}
 ret=sbk_pte_arm(current->mm,r.address,r.pages,ids);
 if(r.mode==1)ids[1]=saved;
 if(r.mode==2)ids[r.pages/2]=saved;
out:
 for(i=0;i<created;i++)sbk_pte_token_put(ids[i]);
 kfree(ids);return ret;
}
static const struct file_operations ops={.owner=THIS_MODULE,.unlocked_ioctl=test_ioctl};
static struct miscdevice dev={.minor=MISC_DYNAMIC_MINOR,.name="sbk_bridge_test",.fops=&ops,.mode=0600};
static int __init test_init(void){return misc_register(&dev);}
static void __exit test_exit(void){misc_deregister(&dev);}
module_init(test_init);module_exit(test_exit);MODULE_LICENSE("GPL");
