#!/usr/bin/env bash
# Checks that this machine can be a ParaMesh node and can run the project's tests.
#
#   tools/check_env.sh [-v] [-k]
#
#   -v   also show what each probe printed
#   -k   keep the temporary directory with the probe and its builds
#
# Prints one line per item: "pass", "FAIL" or "n/a". Exits 0 if nothing failed, 1 otherwise,
# 2 for a wrong option. Run it as an ordinary user: "without root" is one of the checks.
# It needs a C compiler (cc, gcc or clang; set CC to choose) because the kernel checks are a
# small C program, built and run here.

set -u

VERBOSE=0
KEEP=0
while getopts "vkh" opt; do
    case "$opt" in
    v) VERBOSE=1 ;;
    k) KEEP=1 ;;
    h)
        sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    *)
        echo "usage: $0 [-v] [-k]" >&2
        exit 2
        ;;
    esac
done

REGION_BASE="0x600000000000"
MIN_KERNEL_MAJOR=5
MIN_KERNEL_MINOR=19
MAP_ROUNDS=20      # the address check is repeated: address-space layout changes per run
PROBE_TIMEOUT=20   # seconds

WORK="$(mktemp -d "${TMPDIR:-/tmp}/paramesh-check.XXXXXX")" || {
    echo "cannot create a temporary directory" >&2
    exit 1
}
cleanup() {
    if [ "$KEEP" -eq 1 ]; then
        echo "kept: $WORK"
    else
        rm -rf "$WORK"
    fi
}
trap cleanup EXIT

FAILS=0
pass() { printf 'pass  %s\n' "$1"; }
fail() {
    FAILS=$((FAILS + 1))
    printf 'FAIL  %s\n' "$1"
    [ $# -gt 1 ] && printf '      %s\n' "$2"
    return 0
}
skip() {
    printf 'n/a   %s\n' "$1"
    [ $# -gt 1 ] && printf '      %s\n' "$2"
    return 0
}
detail() {
    # Shows a probe's output under -v, and always its first line after a failure.
    local file="$1" always="$2"
    [ -s "$file" ] || return 0
    if [ "$VERBOSE" -eq 1 ]; then
        sed 's/^/      | /' "$file"
    elif [ "$always" -eq 1 ]; then
        head -n 1 "$file" | sed 's/^/      | /'
    fi
}

# ------------------------------------------------------------------------------ the probe

cat >"$WORK/probe.c" <<'PROBE'
/* ParaMesh environment probe.
 *   probe map    map the 4 GB region at its fixed address
 *   probe uffd   user-mode userfaultfd, missing and write-protect faults, on a small mapping
 *   probe full   both together: faults on the first and last page of the region
 * Exit status: 0 pass; 10 userfaultfd refused; 11 UFFDIO_API failed; 12 no write-protect
 * feature; 20 region address not available; 30 register failed; 31 register lacks an ioctl;
 * 40, 41, 43 the missing-fault path failed; 42, 44 to 49 the write-protect path failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define BASE ((void *)0x600000000000ULL)
#define REGION (4ULL << 30)
#define PAGE 4096UL

static volatile unsigned char *g_page;
static volatile int g_read_value = -1;

static void *toucher(void *arg) {
    (void)arg;
    g_read_value = g_page[0]; /* missing fault */
    g_page[1] = 0x5A;         /* write-protect fault */
    return NULL;
}

static int wait_fault(int uffd, struct uffd_msg *msg) {
    struct pollfd p = {.fd = uffd, .events = POLLIN};
    if (poll(&p, 1, 3000) != 1) return -1;
    if (read(uffd, msg, sizeof *msg) != (ssize_t)sizeof *msg) return -1;
    return msg->event == UFFD_EVENT_PAGEFAULT ? 0 : -1;
}

static int fault_test(int uffd, unsigned char *page) {
    static unsigned char src[PAGE];
    struct uffd_msg msg;
    pthread_t t;

    memset(src, 0xAB, sizeof src);
    g_page = page;
    g_read_value = -1;
    if (pthread_create(&t, NULL, toucher, NULL) != 0) return 40;

    if (wait_fault(uffd, &msg) != 0) return 41;
    if (msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP) return 42;
    struct uffdio_copy copy = {.dst = (unsigned long)page, .src = (unsigned long)src, .len = PAGE,
                               .mode = UFFDIO_COPY_MODE_WP};
    if (ioctl(uffd, UFFDIO_COPY, &copy) != 0) return 43;

    if (wait_fault(uffd, &msg) != 0) return 44;
    if (!(msg.arg.pagefault.flags & UFFD_PAGEFAULT_FLAG_WP)) return 45;
    struct uffdio_writeprotect wp = {.range = {.start = (unsigned long)page, .len = PAGE}, .mode = 0};
    if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) != 0) return 46;

    if (pthread_join(t, NULL) != 0) return 47;
    if (g_read_value != 0xAB) return 48;
    if (page[1] != 0x5A || page[2] != 0xAB) return 49;
    return 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "full";
    unsigned char *area;
    unsigned long long size;

    if (strcmp(mode, "uffd") == 0) {
        size = 16 * PAGE;
        area = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (area == MAP_FAILED) {
            fprintf(stderr, "mmap of 16 pages: %s\n", strerror(errno));
            return 30;
        }
    } else {
        size = REGION;
        area = mmap(BASE, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
        if (area != (unsigned char *)BASE) {
            fprintf(stderr, "mmap of 4 GB at %p returned %p: %s\n", BASE, (void *)area, strerror(errno));
            return 20;
        }
        if (strcmp(mode, "map") == 0) return 0;
    }

    int uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    if (uffd < 0) {
        fprintf(stderr, "userfaultfd(UFFD_USER_MODE_ONLY): %s\n", strerror(errno));
        return 10;
    }
    struct uffdio_api api = {.api = UFFD_API, .features = UFFD_FEATURE_PAGEFAULT_FLAG_WP};
    if (ioctl(uffd, UFFDIO_API, &api) != 0) {
        fprintf(stderr, "UFFDIO_API with the write-protect feature: %s\n", strerror(errno));
        return errno == EINVAL ? 12 : 11;
    }
    if (!(api.features & UFFD_FEATURE_PAGEFAULT_FLAG_WP)) {
        fprintf(stderr, "the kernel does not offer UFFD_FEATURE_PAGEFAULT_FLAG_WP\n");
        return 12;
    }

    madvise(area, size, MADV_NOHUGEPAGE);
    struct uffdio_register reg = {.range = {.start = (unsigned long)area, .len = size},
                                  .mode = UFFDIO_REGISTER_MODE_MISSING | UFFDIO_REGISTER_MODE_WP};
    if (ioctl(uffd, UFFDIO_REGISTER, &reg) != 0) {
        fprintf(stderr, "UFFDIO_REGISTER for missing and write-protect: %s\n", strerror(errno));
        return 30;
    }
    const uint64_t need = (1ULL << _UFFDIO_COPY) | (1ULL << _UFFDIO_WAKE) | (1ULL << _UFFDIO_WRITEPROTECT);
    if ((reg.ioctls & need) != need) {
        fprintf(stderr, "the registered range lacks UFFDIO_COPY, UFFDIO_WAKE or UFFDIO_WRITEPROTECT\n");
        return 31;
    }

    int rc = fault_test(uffd, area);
    if (rc == 0) rc = fault_test(uffd, area + size - PAGE);
    if (rc != 0) fprintf(stderr, "fault test failed at step %d\n", rc);
    return rc;
}
PROBE

# build <output> <compiler> [flags...]: 0 if the probe was built.
build() {
    local out="$1" cc="$2"
    shift 2
    "$cc" -std=c11 -O1 -g "$@" -o "$WORK/$out" "$WORK/probe.c" -lpthread >"$WORK/$out.build" 2>&1
}
# run <binary> <mode>: the probe's exit status; its output goes to <binary>.<mode>.out
# The subshell keeps the shell's own "Aborted" notice, for a probe that dies, off the screen.
run() {
    (
        timeout "$PROBE_TIMEOUT" "$WORK/$1" "$2" >"$WORK/$1.$2.out" 2>&1
        exit $?
    ) 2>/dev/null
}

# ------------------------------------------------------------------------------ the checks

echo "ParaMesh environment check"
echo "  host:    $(uname -n)"
echo "  kernel:  $(uname -sr) $(uname -m)"
if [ -r /etc/os-release ]; then
    echo "  system:  $(. /etc/os-release && echo "${PRETTY_NAME:-unknown}")"
fi
echo "  user:    $(id -un) (uid $(id -u))"
echo

# 1. Kernel version.
kernel="$(uname -r)"
kmajor="${kernel%%.*}"
krest="${kernel#*.}"
kminor="${krest%%[!0-9]*}"
if [ "$(uname -s)" != "Linux" ]; then
    fail "Linux kernel $MIN_KERNEL_MAJOR.$MIN_KERNEL_MINOR or newer" "this is $(uname -s), not Linux"
elif ! [ "$kmajor" -ge 0 ] 2>/dev/null || ! [ "${kminor:-x}" -ge 0 ] 2>/dev/null; then
    fail "Linux kernel $MIN_KERNEL_MAJOR.$MIN_KERNEL_MINOR or newer" "cannot read a version from '$kernel'"
elif [ "$kmajor" -gt "$MIN_KERNEL_MAJOR" ] ||
    { [ "$kmajor" -eq "$MIN_KERNEL_MAJOR" ] && [ "$kminor" -ge "$MIN_KERNEL_MINOR" ]; }; then
    pass "Linux kernel $MIN_KERNEL_MAJOR.$MIN_KERNEL_MINOR or newer (found $kmajor.$kminor)"
else
    fail "Linux kernel $MIN_KERNEL_MAJOR.$MIN_KERNEL_MINOR or newer" "found $kmajor.$kminor; install a newer kernel"
fi

# 2. Page size. ParaMesh moves 4 KB pages.
pagesize="$(getconf PAGESIZE 2>/dev/null || echo unknown)"
if [ "$pagesize" = "4096" ]; then
    pass "4 KB pages"
else
    fail "4 KB pages" "the page size here is $pagesize bytes"
fi

# 3. Not root: the userfaultfd check below must hold for an ordinary user.
if [ "$(id -u)" -eq 0 ]; then
    fail "run as an ordinary user" "this is root; run the script again without sudo"
else
    pass "run as an ordinary user"
fi

# 4. A C compiler, for the probe.
compilers=""
if [ -n "${CC:-}" ]; then
    command -v "$CC" >/dev/null 2>&1 && compilers="$CC"
else
    for c in gcc clang; do
        command -v "$c" >/dev/null 2>&1 && compilers="$compilers $c"
    done
    [ -z "$compilers" ] && command -v cc >/dev/null 2>&1 && compilers="cc"
fi
main_cc=""
for c in $compilers; do
    if build "probe-$c" "$c"; then
        [ -z "$main_cc" ] && main_cc="$c"
    else
        fail "build the probe with $c" "see the compiler's message below"
        detail "$WORK/probe-$c.build" 1
    fi
done
if [ -n "$main_cc" ]; then
    pass "C compiler (${compilers# })"
elif [ -z "$compilers" ] && [ -n "${CC:-}" ]; then
    fail "C compiler" "CC is set to '$CC', which is not installed"
elif [ -z "$compilers" ]; then
    fail "C compiler" "none of gcc, clang or cc found; on Ubuntu: sudo apt install build-essential"
fi

if [ -z "$main_cc" ]; then
    for item in "user-mode userfaultfd without root" "write-protect faults on anonymous memory" \
        "4 GB free at $REGION_BASE" "faults served on the first and last page of the region" \
        "AddressSanitizer with the 4 GB region" "UndefinedBehaviorSanitizer with the 4 GB region"; do
        fail "$item" "not checked: the probe could not be built"
    done
else
    probe="probe-$main_cc"

    # 5 and 6. userfaultfd and write-protect, on a small mapping the kernel places.
    run "$probe" uffd
    rc=$?
    case "$rc" in
    0)
        pass "user-mode userfaultfd without root"
        pass "write-protect faults on anonymous memory"
        ;;
    12 | 31 | 42 | 44 | 45 | 46 | 47 | 48 | 49)
        pass "user-mode userfaultfd without root"
        fail "write-protect faults on anonymous memory" "probe exit $rc; needs userfaultfd write-protect (Linux 5.7 or newer)"
        detail "$WORK/$probe.uffd.out" 1
        ;;
    124)
        fail "user-mode userfaultfd without root" "the probe did not finish in $PROBE_TIMEOUT s"
        fail "write-protect faults on anonymous memory" "not reached"
        ;;
    *)
        fail "user-mode userfaultfd without root" "probe exit $rc; a seccomp profile (containers) or an old kernel can block it"
        detail "$WORK/$probe.uffd.out" 1
        fail "write-protect faults on anonymous memory" "not reached"
        ;;
    esac

    # 7. The region's address, over several runs.
    bad=0
    i=0
    while [ "$i" -lt "$MAP_ROUNDS" ]; do
        run "$probe" map || bad=$((bad + 1))
        i=$((i + 1))
    done
    if [ "$bad" -eq 0 ]; then
        pass "4 GB free at $REGION_BASE ($MAP_ROUNDS of $MAP_ROUNDS runs)"
    else
        fail "4 GB free at $REGION_BASE" "taken in $bad of $MAP_ROUNDS runs"
        detail "$WORK/$probe.map.out" 1
    fi

    # 8. Everything together on the real region.
    run "$probe" full
    rc=$?
    if [ "$rc" -eq 0 ]; then
        pass "faults served on the first and last page of the region"
    else
        fail "faults served on the first and last page of the region" "probe exit $rc"
        detail "$WORK/$probe.full.out" 1
    fi

    # 9. Sanitizers with the region, for every compiler found.
    for c in $compilers; do
        [ -x "$WORK/probe-$c" ] || continue
        for san in address undefined; do
            case "$san" in
            address) name="AddressSanitizer" ;;
            *) name="UndefinedBehaviorSanitizer" ;;
            esac
            if ! build "probe-$c-$san" "$c" "-fsanitize=$san" -fno-sanitize-recover=all -fno-omit-frame-pointer; then
                fail "$name with the 4 GB region ($c)" "could not build with -fsanitize=$san; is the sanitizer runtime installed?"
                detail "$WORK/probe-$c-$san.build" 1
                continue
            fi
            run "probe-$c-$san" full
            rc=$?
            if [ "$rc" -eq 0 ]; then
                pass "$name with the 4 GB region ($c)"
            else
                fail "$name with the 4 GB region ($c)" "probe exit $rc"
                detail "$WORK/probe-$c-$san.full.out" 1
            fi
        done
    done
    skip "ThreadSanitizer with the 4 GB region" \
        "not possible on any machine: $REGION_BASE is outside the addresses ThreadSanitizer lets a program map. Tests that map the region run without it."
fi

# 10. Network namespaces and tc netem, for the multi-node tests.
missing=""
for tool in ip tc unshare; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
netem_test='ip link set lo up && tc qdisc add dev lo root netem delay 5ms loss 1% && tc qdisc show dev lo | grep -q netem'
if [ -n "$missing" ]; then
    fail "network namespace with tc netem" "missing:$missing; on Ubuntu: sudo apt install iproute2 util-linux"
elif unshare --user --map-root-user --net sh -c "$netem_test" >"$WORK/netns.out" 2>&1; then
    pass "network namespace with tc netem (without root)"
elif sudo -n true 2>/dev/null && sudo -n unshare --net sh -c "$netem_test" >"$WORK/netns-sudo.out" 2>&1; then
    pass "network namespace with tc netem (through sudo)"
else
    fail "network namespace with tc netem" \
        "neither an unprivileged user namespace nor passwordless sudo could create one with netem"
    detail "$WORK/netns.out" 1
fi

echo
if [ "$FAILS" -eq 0 ]; then
    echo "All checks passed."
    exit 0
fi
echo "$FAILS check(s) failed."
exit 1
