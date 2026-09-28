/* RXE source pre-registration with a distinct live process mm. The source
 * controller must never reuse a descriptor after that process unmaps it. */
static void test_source_prearm(void)
{
    int ready[2], command[2];
    CHECK(!pipe(ready) && !pipe(command));
    int fd = export_source();
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(ready[0]); close(command[1]);
        unsigned char *p = mmap(NULL, 64 * P, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(p != MAP_FAILED && !madvise(p, 64 * P, MADV_NOHUGEPAGE));
        memset(p, 0x5d, 64 * P);
        uintptr_t address = (uintptr_t)p;
        CHECK(write(ready[1], &address, sizeof(address)) == sizeof(address));
        char event;
        CHECK(read(command[0], &event, 1) == 1 && event == 'U');
        CHECK(!munmap(p, 64 * P));
        CHECK(write(ready[1], "D", 1) == 1);
        CHECK(read(command[0], &event, 1) == 1 && event == 'Q');
        close(ready[1]); close(command[0]);
        _exit(0);
    }
    close(ready[1]); close(command[0]);
    uintptr_t address;
    CHECK(read(ready[0], &address, sizeof(address)) == sizeof(address));
    struct sbk_capabilities caps;
    CHECK(!ioctl(fd, SBK_IOC_CAPABILITIES, &caps) &&
          (caps.features & SBK_FEATURE_REMOTE_PREARM));
    struct sbk_prearm_batch request = {.pid = child,
                                     .batch = {.count = 1, .workers = 1}};
    request.batch.regions[0] = (struct sbk_rdma_region){.address = address, .pages = 64};
    CHECK(!ioctl(fd, SBK_IOC_PREARM_BATCH, &request));
    CHECK(request.batch.completed == 1 && request.batch.regions[0].id &&
          request.batch.regions[0].rkey);
    struct sbk_prearm_status status = {.region = request.batch.regions[0]};
    CHECK(!ioctl(fd, SBK_IOC_PREARM_STATUS, &status) && status.valid);
    CHECK(write(command[1], "U", 1) == 1);
    char event;
    CHECK(read(ready[0], &event, 1) == 1 && event == 'D');
    status.valid = 0;
    CHECK(!ioctl(fd, SBK_IOC_PREARM_STATUS, &status) && !status.valid);
    CHECK(!ioctl(fd, SBK_IOC_REVOKE_SOURCE));
    CHECK(write(command[1], "Q", 1) == 1);
    int exit_status;
    CHECK(waitpid(child, &exit_status, 0) == child && WIFEXITED(exit_status) &&
          !WEXITSTATUS(exit_status));
    CHECK(!close(fd) && !close(ready[0]) && !close(command[1]));
    puts("PASS RDMA_source_prearm remote_mm=1 invalidation=1 revoke=1");
}
