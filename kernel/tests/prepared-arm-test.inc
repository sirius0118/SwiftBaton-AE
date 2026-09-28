/* SPDX-License-Identifier: GPL-2.0 */
/* PS prepares before any destination VMA exists, then dirty invalidation and
 * final ARM use the same real module context. The separate kernel fixture
 * exercises partial table-publication injection and cgroup accounting. */
static void test_prepared_arm_lifetime(void)
{
    for (int which = 0; which < 4; which++) {
        defer_mapping = 1;
        struct fixture f = fixture(64, 1, 2, 0, SBK_NO_FAILURE);
        defer_mapping = 0;
        struct sbk_capabilities caps = {0};
        CHECK(!ioctl(f.fd, SBK_IOC_CAPABILITIES, &caps));
        CHECK(caps.features & SBK_FEATURE_PREPARED_ARM);
        unsigned char *address = mmap(NULL, f.n * P, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(address != MAP_FAILED && !munmap(address, f.n * P));
        uint64_t pages = f.n;
        CHECK(!ioctl(f.fd, SBK_IOC_TOKEN_RESERVE, &pages));
        struct sbk_anon_arm a = {.address = (uintptr_t)address};
        CHECK(!ioctl(f.fd, SBK_IOC_PREPARE_ANON, &a));
        CHECK(ioctl(f.fd, SBK_IOC_PREPARE_ANON, &a) < 0 && errno == EINVAL);
        struct sbk_drain_status d = {0};
        CHECK(!ioctl(f.fd, SBK_IOC_DRAIN_STATUS, &d));
        CHECK(!d.armed && !d.retired_tokens);
        CHECK(mmap(NULL, f.n * P, PROT_READ, MAP_PRIVATE, f.fd, 0) == MAP_FAILED);
        if (which < 2) {
            if (which == 1) CHECK(!ioctl(f.fd, SBK_IOC_CANCEL));
            finish(&f);
            printf("PASS PREPARED_ARM_%s_before_destination_mm\n", which ? "cancel" : "close");
            continue;
        }
        uint64_t pre[] = {3, 4, 7, 9}, dirty[] = {4};
        struct sbk_hot_list l = {.indices = (uintptr_t)pre, .count = 4};
        CHECK(!ioctl(f.fd, SBK_IOC_PRETRANSFER, &l));
        CHECK(ioctl(f.fd, SBK_IOC_ARM_ANON, &a) < 0 && errno == EINVAL);
        f.src[4 * P] ^= 0x5a;
        l.indices = (uintptr_t)dirty; l.count = 1;
        CHECK(!ioctl(f.fd, SBK_IOC_SEAL, &l));
        f.dst = mmap(address, f.n * P, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        CHECK(f.dst == address);
        if (which == 3) {
            f.dst[40 * P] = 0x76;
            CHECK(ioctl(f.fd, SBK_IOC_ARM_ANON, &a) < 0 && errno == EEXIST);
            CHECK(f.dst[40 * P] == 0x76);
            CHECK(ioctl(f.fd, SBK_IOC_ARM_ANON, &a) < 0 && errno == EINVAL);
            finish(&f); puts("PASS PREPARED_ARM_occupied_failure_requires_fresh_context");
            continue;
        }
        struct sbk_anon_arm wrong = {.address = a.address + P};
        CHECK(ioctl(f.fd, SBK_IOC_ARM_ANON, &wrong) < 0 && errno == ESTALE);
        CHECK(!ioctl(f.fd, SBK_IOC_ARM_ANON, &a));
        CHECK(!close(f.fd)); f.fd = -1;
        CHECK(!memcmp(f.dst, f.src, f.n * P));
        finish(&f); puts("PASS PREPARED_ARM_PS_dirty_seal_close_before_fault");
    }
    puts("SBK_PREPARED_LIFETIME_PASS");
}
