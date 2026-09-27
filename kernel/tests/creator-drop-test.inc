/* Real anonymous-PTE lifetime test with a LOOPBACK-only delayed worker. */
static void test_creator_drop_lifetime(void)
{
    CHECK(use_anon && use_token_pool);
    FILE *param = fopen("/sys/module/swiftbaton_k/parameters/creator_drop_test_delay_ms", "r");
    unsigned int delay = 0;
    CHECK(param && fscanf(param, "%u", &delay) == 1 && !fclose(param));
    CHECK(delay >= 500);
    for (int mode = 0; mode < 6; mode++) {
        defer_mapping = 1;
        struct fixture f = fixture(64, 1, 4, mode == 3 ? 2000 : 0, SBK_NO_FAILURE);
        defer_mapping = 0;
        if (mode == 5) {
            uint64_t pages = f.n;
            CHECK(!ioctl(f.fd, SBK_IOC_TOKEN_RESERVE, &pages));
            unsigned char *bad = mmap(NULL, f.n * P, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            CHECK(bad != MAP_FAILED);
            bad[32 * P] = 123; /* Occupied destination: ARM must fail before publishing markers. */
            struct sbk_anon_arm arm = {.address = (uintptr_t)bad};
            CHECK(ioctl(f.fd, SBK_IOC_ARM_ANON, &arm) < 0 && errno == EEXIST);
            CHECK(bad[0] == 0 && bad[32 * P] == 123 && bad[63 * P] == 0);
            CHECK(!munmap(bad, f.n * P));
        }
        f.dst = map_destination(f.fd, f.n);
        int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        CHECK(event >= 0 && !ioctl(f.fd, SBK_IOC_WATCH_DRAIN, &event));
        if (mode == 1) verify(&f);
        if (mode == 3) { background(f.fd, NULL, 0); usleep(1000); }
        if (mode == 4) CHECK(!ioctl(f.fd, SBK_IOC_CANCEL));
        if (mode == 2) {
            /* Both fork copies must work after the only device fd closes. */
            CHECK(!close(f.fd)); f.fd = -1;
            pid_t child = fork(); CHECK(child >= 0);
            if (!child) { verify(&f); finish(&f); _exit(0); }
            int status;
            CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
            verify(&f);
        }
        CHECK(!munmap(f.dst, f.n * P)); f.dst = NULL;
        if (f.fd >= 0) {
            struct sbk_drain_status status = {0};
            CHECK(!ioctl(f.fd, SBK_IOC_DRAIN_STATUS, &status));
            /* Distinguish a pending creator owner from a merely queued ACK. */
            CHECK(status.armed && !status.drained && !status.retired_tokens);
            CHECK(!close(f.fd)); f.fd = -1;
        }
        finish(&f);
        struct pollfd p = {.fd = event, .events = POLLIN};
        CHECK(poll(&p, 1, 10000) == 1 && p.revents == POLLIN);
        uint64_t signals;
        CHECK(read(event, &signals, sizeof(signals)) == sizeof(signals) && signals == 1);
        CHECK(read(event, &signals, sizeof(signals)) < 0 && errno == EAGAIN);
        CHECK(!close(event));
        printf("PASS creator_drop_pending_close_drain mode=%d\n", mode);
    }
    puts("SBK_CREATOR_LIFETIME_PASS close unmap fork fault background cancel occupied_ARM_retry");
}
