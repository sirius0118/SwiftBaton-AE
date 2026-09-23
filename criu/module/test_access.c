#include <string.h>
#include <stdio.h>
#include <stdlib.h>


#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>

// #include <linux/bitops.h>  // 基础位操作声明
// #include <asm/bitops.h>    // 特定体系结构的实现（某些旧内核可能需要）
#include "test_access.h"
#include "types.h"
#include <string.h>
#include <fcntl.h>   // O_RDWR 在这里定义
#include <unistd.h>  // open/close
#include <stdbool.h>
#include "common/arch/x86/asm/cmpxchg.h"
#include "common/arch/x86/asm/asm.h"
#include "common/asm/bitsperlong.h"

struct ioctl_data
{
    int pid;
    unsigned long addr;
    // char *addr;
    // char *path;
};

static inline bool test_bit(long nr, volatile unsigned long *addr)
{
	bool oldbit;

	asm volatile(__ASM_SIZE(bt) " %2,%1" CC_SET(c)
		     : CC_OUT(c)(oldbit)
		     : "m"(*(unsigned long *)addr), "Ir"(nr)
		     : "memory");

	return oldbit;
}

// #define test_bit(bit, bitmap) ((*bitmap) & (1UL << (bit)))
#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
#define MAX_PAGES_NUM	   10 * 1024 * 1024
#define SHARED_MEM_SIZE	   4096 * 4096 * 20
#define IOCTL_MODIFY_PTE   _IOW('m', 1, struct ioctl_data)
#define IOCTL_ALLOC_MEMORY _IOW('m', 2, int)
#define IOCTL_FREE_MEMORY  _IOW('m', 3, int)
#define SAMPLE_TIMES	   10
#define SAMPLE_INTERVAL	   2
#define PRIORITY_QUEUE_LEVEL 20
#define ACCESS_VMA_SIZE 409600

int sl_size = 0;

struct all_vma_area
{
    unsigned long start;
    unsigned long end;
    unsigned long size;
    unsigned long *bitmap;
    struct score_list *score;

    struct all_vma_area *prev;
    struct all_vma_area *next;
};

#define NEW_VMA(vma)                                                  \
    vma = (struct all_vma_area *)malloc(sizeof(struct all_vma_area)); \
    if (!vma)                                                         \
        return NULL;                                                  \
    memset(vma, 0, sizeof(struct all_vma_area));


float min_score=-10,max_score=-10;
struct score_list **DL_multihead;
// TODO:需要确定方案
static inline float score_policy(float score, int index, int times)
{
    // return score+1;
    return score+(float)index / times;
}

// 交换双向链表中的两个节点
void swap_nodes(struct score_list **head, struct score_list *node1, struct score_list *node2)
{
    struct score_list *prev1 = (struct score_list *)node1->prev;
    struct score_list *next1 = (struct score_list *)node1->next;
    struct score_list *prev2 = (struct score_list *)node2->prev;
    struct score_list *next2 = (struct score_list *)node2->next;
    if (node1 == node2)
        return;

    

    // 处理相邻节点的情况
    if (next1 == node2)
    {
        node1->next = next2;
        node1->prev = node2;
        node2->next = node1;
        node2->prev = prev1;
        if (prev1)
            prev1->next = node2;
        if (next2)
            next2->prev = node1;
    }
    else if (next2 == node1)
    {
        node2->next = next1;
        node2->prev = node1;
        node1->next = node2;
        node1->prev = prev2;

        if (next1)
            next1->prev = node2;
        if (prev2)
            prev2->next = node1;
    }
    else
    {
        node1->next = next2;
        node1->prev = prev2;
        node2->next = next1;
        node2->prev = prev1;
        if (prev1)
            prev1->next = node2;
        if (next1)
            next1->prev = node2;
        if (prev2)
            prev2->next = node1;
        if (next2)
            next2->prev = node1;
    }

    if (node1->prev == NULL)
        *head = node1;
    if (node2->prev == NULL)
        *head = node2;
}

struct score_list *score_list_sort(struct score_list *list)
{
    // sort the score list by score
    struct score_list *head = list;
    struct score_list *cur_i = list;
    struct score_list *cur_j = list;
    struct score_list *temp = NULL;
    while (cur_i != NULL)
    {
        cur_j = (struct score_list *)cur_i->next;
        while (cur_j != NULL)
        {
            // printf("i_addr:%p j_addr:%p\n", cur_i->addr, cur_j->addr);
            if (cur_i->score < cur_j->score)
            {
                swap_nodes(&head, cur_i, cur_j);
                temp = cur_i;
                cur_i = cur_j;
                cur_j = temp;
                // printf("i_addr:%p j_addr:%p exchange\n", cur_i->addr, cur_j->addr);
            }
            cur_j = (struct score_list *)cur_j->next;
        }
        cur_i = (struct score_list *)cur_i->next;
    }

    return head;
}

int is_exist(struct all_vma_area *vma_head, struct all_vma_area *vma)
{
    struct all_vma_area *temp = vma_head;
    while (temp != NULL)
    {
        if (temp->start == vma->start && temp->end == vma->end)
        {
            return 1;
        }
        temp = temp->next;
    }
    return 0;
}

void print_info(struct epoch_area *epoch_area)
{
	int ret = 0, i, j, k;
	struct access_area *access_area;
	struct all_vma_area *vma_tail = NULL, *vma, *vma_head = NULL;
	struct __vma_area *vml;
	int score_list_size = 0;
	int epoch_i, access_i, vma_i;
	int all_vma_num = 0;
    FILE *print_log;

	struct score_list *Scorelist, *head = NULL;
    // printf("print printlog\n");
    print_log = fopen("/var/lib/criu/print.log", "w");
	// ----------------- step2: collect access -----------------
	// judge whether the vma_area is overlap, then compute the score
	for (epoch_i = 0; epoch_i < epoch_area->num_area; epoch_i++) {
		access_area = epoch_area->areas[epoch_i];
		for (j = 0; j < access_area->num_vma; j++) {
			vml = (struct __vma_area *)((void *)access_area + sizeof(struct access_area) + sizeof(struct __vma_area) * j);
			unsigned long *bitmap = vml->bitmap;
			unsigned long *dirty_bitmap = vml->dirty_bitmap;
			for (k = 0; k < (vml->end - vml->start) / PAGE_SIZE; k++) {
				if (test_bit(k,bitmap)) {
                    fprintf(print_log, "%lx access\n",vml->start+k*PAGE_SIZE);
                }
                if (test_bit(k,dirty_bitmap)) {
                    fprintf(print_log, "%lx dirty\n",vml->start+k*PAGE_SIZE);
                }
			}
		}
	}
    fclose(print_log);
}

struct score_list *analyze_access(struct epoch_area *epoch_area, int access_hot, int dirty_hot, volatile unsigned long *read_hot_list, volatile int *read_hot_num)
{
    int ret = 0, i, j;
    struct access_area *access_area;
    struct all_vma_area *vma_tail = NULL, *vma, *vma_head = NULL;
    struct __vma_area *vml;
    unsigned long score_list_size = 0;
    int epoch_i, access_i, vma_i;
    int all_vma_num = 0;
    unsigned long start, end;

    volatile struct score_list *Scorelist, *head = NULL;
    printf("analyze_access: enter\n");

    // ----------------- step1: merge all vma -----------------
    // 多轮有相同的vma，已去重
    // 实际不需要去重，由于vma会发生变化（并不是单纯的新增）所以以最新一轮为准即可
    for (i = epoch_area->num_area-1; i < epoch_area->num_area; i++)
    {
        access_area = epoch_area->areas[i];
        printf("**********************************access:%d***************************************\n",i);
        for (j = 0; j < access_area->num_vma; j++)
        {
            NEW_VMA(vma);
            vml = (struct __vma_area *)((void *)access_area + sizeof(struct access_area) + sizeof(struct __vma_area) * j);
            vma->start = vml->start;
            vma->end = vml->end;
            // if (is_exist(vma_head, vma))
            // {
            //     free(vma);
            //     continue;
            // }
            printf("vma start:%lx vma end:%lx\n", vma->start,vma->end);
            vma->size = vml->size;
            score_list_size += (int)(vma->size / PAGE_SIZE);
            vma->bitmap = vml->bitmap;
            if (!vma_tail)
            {
                vma->next = NULL;
                vma->prev = NULL;
                vma_tail = vma;
                vma_head = vma;
            }
            else
            {
                // insert to the tail
                vma_tail->next = vma;
                vma->prev = vma_tail;
                vma_tail = vma;
                vma_tail->next = NULL;
            }
        }
    }
    printf("analyze_access: vma list create success!\n");
    Scorelist = (struct score_list *)malloc(sizeof(struct score_list) * score_list_size);
    memset((void *)Scorelist, 0, sizeof(struct score_list) * score_list_size);

    // init all vma area
    score_list_size = 0;
    vma = vma_head;
    while (vma != NULL)
    {
        unsigned long addr;
        vma->score = (struct score_list *)(Scorelist + score_list_size);
        score_list_size += (uint64_t)(vma->size / PAGE_SIZE);

        addr = vma->start;
        printf("vma start:%lx vma end:%lx\n", vma->start,vma->end);
        for (i = 0; i < (uint64_t)(vma->size / PAGE_SIZE); i++)
        {
            // vma->score[i].addr = addr + PAGE_SIZE;//?
            vma->score[i].addr = addr;
            addr += PAGE_SIZE;
        }
        all_vma_num++;
        vma = vma->next;
    }
    printf("analyze_access: vma scorelist success: %ld\n", score_list_size);
    // for(i=0;i<score_list_size;i++){
    //     printf("scorelist[%d]:%lx\n",i,Scorelist[i].addr);
    // }

    for (i = 0; i < score_list_size; i++)
    {
        if (i == 0)
        {
            Scorelist[i].prev = NULL;
            Scorelist[i].next = &Scorelist[i + 1];
        }
        else if (i == score_list_size - 1)
        {
            Scorelist[i].prev = &Scorelist[i - 1];
            Scorelist[i].next = NULL;
        }
        else
        {
            Scorelist[i].prev = &Scorelist[i - 1];
            Scorelist[i].next = &Scorelist[i + 1];
        }
    }

    // ----------------- step2: collect access -----------------
    // judge whether the vma_area is overlap, then compute the score
    for (epoch_i = 0; epoch_i < epoch_area->num_area; epoch_i++)
    {
        access_area = epoch_area->areas[epoch_i];
        vma = vma_head;
        j = 0;
        vml = (struct __vma_area *)((void *)access_area + sizeof(struct access_area) + sizeof(struct __vma_area) * j);
        while (vma != NULL && j < access_area->num_vma)
        {
            
            start = vma->start > vml->start ? vma->start : vml->start;
            end = vma->end < vml->end ? vma->end : vml->end;
            if (start <= end)
            {
                // [start:end] is the overlap area
                unsigned long *bitmap = vml->bitmap;
                unsigned long *dirty_bitmap = vml->dirty_bitmap;
                int offset = (start - vml->start) / PAGE_SIZE;
                for (i = 0; i < (end - start) / PAGE_SIZE; i++)
                {
                    // if (ca_bitmap_get(bitmap, offset + i))
                    if (test_bit(offset + i, bitmap))
                    {
                        vma->score[offset + i].times++;
                        // TODO:需要根据策略修改计算的值
                        vma->score[offset + i].score = score_policy(vma->score[offset + i].score, epoch_i, epoch_area->num_area);
                        // if(epoch_i==epoch_area->num_area-1){
                        //     if(min_score<0){
                        //         printf("min_score<0\n");
                        //         min_score=vma->score[offset + i].score;
                        //         max_score=vma->score[offset + i].score;
                        //     }
                        //     if(vma->score[offset + i].score<min_score){
                        //         min_score=vma->score[offset + i].score;
                        //     }
                        //     if(vma->score[offset + i].score>max_score){
                        //         max_score=vma->score[offset + i].score;
                        //     }
                        // }
                    }
                    // if (ca_bitmap_get(dirty_bitmap, offset + i))
                    if (test_bit(offset + i, dirty_bitmap))
                    {
                        vma->score[offset + i].dirty_times++;
                    }
                }
            }
            if (vma->end < vml->end)
            {
                vma = vma->next;
            }
            else
            {
                j++;
                vml = (struct __vma_area *)((void *)access_area + sizeof(struct access_area) + sizeof(struct __vma_area) * j);
            }
        }
    }
    printf("analyze_access: count score success: score_list_size:%ld\n", score_list_size);
    sl_size = score_list_size;
    for (i = 0; i < score_list_size; i++)
    {//we need to pretransfer read hot page and all cold page, so we don't delete it
        // if (Scorelist[i].times == 0 && Scorelist[i].dirty_times == 0)
        // {
            
        //     sl_size--;
        //     if (Scorelist[i].prev != NULL)
        //     {
        //         Scorelist[i].prev->next = Scorelist[i].next;
        //     }
        //     if (Scorelist[i].next != NULL)
        //     {
        //         Scorelist[i].next->prev = Scorelist[i].prev;
        //     }
        // }
        // else 
        if (Scorelist[i].times >= access_hot && Scorelist[i].dirty_times <= dirty_hot)
        {
            read_hot_list[*read_hot_num] = Scorelist[i].addr;
            // if(Scorelist[i].addr > 0x7f67b66d5000)
                // printf("read hot page addr:%lx\n", Scorelist[i].addr);
            (*read_hot_num)++;
            sl_size--;
            if (Scorelist[i].prev != NULL)
            {
                Scorelist[i].prev->next = Scorelist[i].next;
            }
            if (Scorelist[i].next != NULL)
            {
                Scorelist[i].next->prev = Scorelist[i].prev;
            }
            Scorelist[i].prev = NULL;
            Scorelist[i].next = NULL;
        }
        else if (head == NULL)
        {
            head = &Scorelist[i];
            if(min_score<0){
                printf("min_score<0\n");
                min_score=Scorelist[i].score;
                max_score=Scorelist[i].score;
                printf("run here\n");
            }
        }else{
            if(Scorelist[i].score<min_score){
                min_score=Scorelist[i].score;
            }
            if(Scorelist[i].score>max_score){
                max_score=Scorelist[i].score;
            }
        }
    }
    printf("read hot page num:%d, 2level:%d\n", *read_hot_num, sl_size);
    printf("analyze_access: prepare sort list of length %ld\n", score_list_size);

    // printf("analyze_access: sort success\n");
    return (struct score_list *)head;
}

struct score_list *get_dirty_list(struct score_list *Scorelist)
{
    volatile struct score_list *dirty_list = NULL;
    // struct score_list *dirty_list_tail = NULL;
    volatile struct score_list *cur = Scorelist;
    volatile struct score_list *temp;
    int num = 0;
    int DL_index=0;
    // 删除怎么遍历
    printf("max_score:%f min_score:%f\n",max_score,min_score);
    while (cur != NULL)
    {
        // printf("cur->score:%f \n",cur->score);
        cur->score=(cur->score-min_score)/(max_score-min_score)*PRIORITY_QUEUE_LEVEL;
        DL_index=(int)cur->score;
        // printf("cur->score:%f DL_index:%d\n",cur->score,DL_index);
        DL_index=DL_index>PRIORITY_QUEUE_LEVEL-1?PRIORITY_QUEUE_LEVEL-1:DL_index;
        if (cur->dirty_times > 1)
        {
            
            temp = (struct score_list *)cur->next;
            num++;
            // delete from the list
            if (cur->next != NULL)
            {
                cur->next->prev = cur->prev;
            }
            if (cur->prev != NULL)
            {
                cur->prev->next = cur->next;
            }
            cur->next = NULL;
            cur->prev = NULL;
            // insert to the dirty list
            if (DL_multihead[DL_index] == NULL)
            {
                DL_multihead[DL_index] = (struct score_list *)cur;
            }
            else
            {
                cur->next = DL_multihead[DL_index];
                DL_multihead[DL_index]->prev = cur;
                DL_multihead[DL_index] = (struct score_list *)cur;
                // dirty_list_tail->next = cur;
                // cur->prev = dirty_list_tail;
                // dirty_list_tail = cur;
            }
            cur = temp;
        }
        else
        {
            // num++;
            // temp=(struct score_list*)malloc(sizeof(struct score_list));
            // *temp=*cur;
            // temp->next = NULL;
            // temp->prev = NULL;
            // // insert to the dirty list
            // if (DL_multihead[DL_index] == NULL)
            // {
            //     DL_multihead[DL_index] = temp;
            // }
            // else
            // {
            //     temp->next = DL_multihead[DL_index];
            //     DL_multihead[DL_index]->prev = temp;
            //     DL_multihead[DL_index] = temp;
            // }
            cur = cur->next;
        }
    }
    printf("dirty list size:%d\n", num);
    // if(dirty_list==NULL){
    //     printf("dirty_list is NULL\n");
    // }
    return (struct score_list *)dirty_list;
}

void address_translation(struct access_area *access)
{
    unsigned long user_bitmap = (unsigned long)access + ACCESS_VMA_SIZE;
    struct __vma_area *first_vma = (struct __vma_area *)((void *)access + sizeof(struct access_area));
    unsigned long kernel_bitmap = (unsigned long)first_vma->bitmap;
    struct __vma_area *vma;
    int i;
    for (i = 0; i < access->num_vma; i++)
    {
        vma = (struct __vma_area *)((void *)access + sizeof(struct access_area) + sizeof(struct __vma_area) * i);
        // printf("trannlation start:%lx bitmap:%lx\n", (uint64_t)vma->start,(uint64_t) vma->bitmap);
        vma->bitmap = (unsigned long *)((unsigned long)vma->bitmap - kernel_bitmap + user_bitmap);
        vma->dirty_bitmap = (unsigned long *)((unsigned long)vma->dirty_bitmap - kernel_bitmap + user_bitmap);
        // printf("trannlation end start:%lx bitmap:%lx data:%lx\n", (uint64_t)vma->start, (uint64_t)vma->bitmap, *(uint64_t*)vma->bitmap);

    }
}

int main(int argc, char *argv[])
{
	int ret, i, fd;
	struct ioctl_data data;
	volatile struct epoch_area *EpochArea;
	volatile struct score_list *ScoreList;
	volatile struct score_list *dirtylist;
	unsigned long *ReadList;
	clock_t start_time, end_time;
	double cpu_time_used;
	int sample_times = 0;
	int sample_intervel = 0;
	char work_dir[100];
	char work_path[100];
	FILE *score_log;
	int read_hot_num;

	int access_hot=1, dirty_hot=1;
	ReadList = (unsigned long *)malloc(30 * 1024 *1024);
	pid_t pid = atoi(argv[1]);
	sample_times = atoi(argv[2]);
	sample_intervel = atoi(argv[3]);
	strcpy(work_dir, argv[4]);
	data.pid = pid;
	start_time = clock();
	EpochArea = (struct epoch_area *)malloc(SHARED_MEM_SIZE);
	// EpochArea = (struct epoch_area *)create_share_memory(&data);
	end_time = clock();

	cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
	printf("malloc time used: %.4f seconds\n", cpu_time_used);
	if (!EpochArea) {
		printf("Failed to create shared memory\n");
		return -1;
	}
	EpochArea = init_epoch_area(EpochArea);

	fd = open("/dev/collect_access", 02);
	if (fd < 0) {
		printf("Failed to open collect_access\n");
		return -1;
	}
	printf("open dev success\n");
	start_time = clock();
	ret = ioctl(fd, IOCTL_ALLOC_MEMORY, 0);
	if (ret < 0) {
		printf("Failed to ioctl init_memory\n");
		return -1;
	}

	end_time = clock();

	cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
	printf("kernel alloc memory time used: %.4f seconds\n", cpu_time_used);

	for (i = 0; i < sample_times; i++) {
		data.addr = (unsigned long)get_mem_point();
		start_time = clock();
		ret = ioctl(fd, IOCTL_MODIFY_PTE, &data);
		if (ret < 0) {
			printf("Failed to ioctl collect_access\n");
			return -1;
		}
		end_time = clock();

		cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
		printf("one access time used: %.4f seconds\n", cpu_time_used);
		start_time = clock();
		address_translation((struct access_area *)get_mem_point());
		end_time = clock();

		cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
		printf("address translation time used: %.4f seconds\n", cpu_time_used);
		update_mem_point(EpochArea, (struct access_area *)get_mem_point());
		// TODO:如果需要更短的时间，需要更换为usleep
		usleep(sample_intervel * 1000);
	}
	start_time = clock();
	ret = ioctl(fd, IOCTL_FREE_MEMORY, 0);
	if (ret < 0) {
		printf("Failed to ioctl init_memory\n");
		return -1;
	}
	end_time = clock();

	cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
	printf("kernel free memory time used: %.4f seconds\n", cpu_time_used);
	printf("ioctl success\n");

	start_time = clock();
    print_info(EpochArea);
	// ScoreList = analyze_access(EpochArea);
	end_time = clock();

	cpu_time_used = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
	printf("analyze time used: %.4f seconds\n", cpu_time_used);
	printf("ioctl success\n");

    printf("collect access success\n");
    ScoreList = analyze_access(EpochArea, access_hot, dirty_hot, ReadList, &read_hot_num);
    printf("read hot num:%d\n", read_hot_num);
    DL_multihead=(struct score_list **)malloc(sizeof(struct score_list *)*PRIORITY_QUEUE_LEVEL);
    memset(DL_multihead,0,sizeof(struct score_list *)*PRIORITY_QUEUE_LEVEL);
    get_dirty_list((struct score_list *)ScoreList);
    //输出scorelist
    sprintf(work_path, "%s/score_%d_%d.log", work_dir, sample_times, sample_intervel);
	printf("address: %s\n", work_path);
    score_log = fopen(work_path, "w");

    // 检查文件是否成功打开
    if (score_log == NULL) {
        printf("无法打开或创建该文件。\n");
        return NULL; // 返回错误码
    }
    for(i=PRIORITY_QUEUE_LEVEL-1;i>=0;i--){
        dirtylist=DL_multihead[i];
        fprintf(score_log, "%d档页面:\n",i);
        while(dirtylist!=NULL){
            fprintf(score_log, "%lx=%lf=%d=%d\n",dirtylist->addr,dirtylist->score,dirtylist->times,dirtylist->dirty_times);
            dirtylist=dirtylist->next;
        }
    }
	fclose(score_log);

	return 0;
}