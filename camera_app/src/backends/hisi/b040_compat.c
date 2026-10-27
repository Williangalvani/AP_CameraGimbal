/*
 * Run-time ABI bridge between OpenIPC's HI3519DV500_MPP_V1.0.1.1 B040 user
 * libraries and the camera's HI3519DV500_MPP_V2.0.1.0 B040 kernel modules.
 * Linked into camera-app and exported (--export-dynamic-symbol), so the MPP
 * libraries bind these definitions ahead of libc's and libss_mpi_sysmem's.
 *
 * 1. ot_isp.ko inserted one ISP ioctl at nr 0x6c (0x4001496c). Every ISP
 *    command from nr 0x6c up is one higher in the kernel than in the
 *    libraries, with the same size and direction (vreg fd bind 0x40044976 is
 *    0x40044977, ISP run 0x8064496f is 0x80644970, ...). libss_mpi.so also
 *    issues type 'I' commands, on other devices, so only /dev/isp_dev fds are
 *    renumbered, and only when /proc/umap/isp reports that module version.
 * 2. ot_isp.ko maps the ISP vreg blocks uncached, so the libraries' cached
 *    mapping is refused by ot_mmz ("prot as cacheable conflit"); fall back to
 *    an uncached mapping.
 * 3. The WDR fusion group ioctl is 48 bytes in both, but the libraries fill
 *    only the first 24 (group and a 20-byte attribute) and send the rest from
 *    uninitialised stack. ot_vi.ko reads pipe_reverse from that tail and
 *    rejects garbage ("invalid reverse_en"); a zero tail is accepted. The
 *    command only writes to the kernel, so a zero-padded copy is sent and the
 *    caller's memory past the first 24 bytes is never touched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define ISP_IOC_TYPE 'I'
#define ISP_IOC_FIRST_SHIFTED 0x6c
#define ISP_IOC_LAST_SHIFTED 0x81
#define ISP_PROC "/proc/umap/isp"
#define ISP_SHIFTED_VERSION "[ISP] Version: [HI3519DV500_MPP_V2.0.1.0 B040 "
#define VI_IOC_SET_FUSION_GRP 0x40304912u
#define VI_FUSION_GRP_FILLED 24
#define VI_FUSION_GRP_SIZE 48

typedef int (*ioctl_fn)(int, int, ...);
typedef void *(*map_fn)(unsigned long long, unsigned int);

static pthread_once_t resolved = PTHREAD_ONCE_INIT;
static ioctl_fn real_ioctl;
static map_fn ss_mmap_cached, ss_mmap, ot_mmap_cached, ot_mmap;
static int isp_shifted;

static void resolve(void)
{
    char version[256] = "";
    FILE *proc;

    real_ioctl = (ioctl_fn)dlsym(RTLD_NEXT, "ioctl");
    ss_mmap_cached = (map_fn)dlsym(RTLD_NEXT, "ss_mpi_sys_mmap_cached");
    ss_mmap = (map_fn)dlsym(RTLD_NEXT, "ss_mpi_sys_mmap");
    ot_mmap_cached = (map_fn)dlsym(RTLD_NEXT, "ot_mpi_sys_mmap_cached");
    ot_mmap = (map_fn)dlsym(RTLD_NEXT, "ot_mpi_sys_mmap");
    // service.sh loads ot_isp.ko before camera-app starts
    proc = fopen(ISP_PROC, "r");
    while (proc != NULL && fgets(version, sizeof(version), proc) != NULL &&
           strncmp(version, "[ISP] Version:", 14) != 0) {
    }
    if (proc != NULL) fclose(proc);
    isp_shifted = strncmp(version, ISP_SHIFTED_VERSION, strlen(ISP_SHIFTED_VERSION)) == 0;
    if (!isp_shifted) {
        version[strcspn(version, "\n")] = '\0';
        fprintf(stderr, "b040_compat: ISP ioctls not renumbered; %s reports \"%s\"\n",
                ISP_PROC, version);
    }
}

static int is_dev(int fd, const char *path)
{
    struct stat dev, st;

    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) return 0;
    if (stat(path, &dev) != 0) return 0;
    return st.st_rdev == dev.st_rdev;
}

/* Every caller passes a pointer or an integer; on AArch64 reading one
 * variadic argument the caller did not pass is harmless. */
int ioctl(int fd, int req, ...)
{
    unsigned int cmd = (unsigned int)req;
    unsigned int nr = cmd & 0xff;
    unsigned char fusion[VI_FUSION_GRP_SIZE];
    va_list ap;
    void *arg;

    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    (void)pthread_once(&resolved, resolve);
    if (real_ioctl == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if (((cmd >> 8) & 0xff) == ISP_IOC_TYPE && nr >= ISP_IOC_FIRST_SHIFTED &&
        nr <= ISP_IOC_LAST_SHIFTED && isp_shifted && is_dev(fd, "/dev/isp_dev")) {
        cmd += 1;
    } else if (cmd == VI_IOC_SET_FUSION_GRP && arg != NULL && is_dev(fd, "/dev/vi")) {
        memcpy(fusion, arg, VI_FUSION_GRP_FILLED);
        memset(fusion + VI_FUSION_GRP_FILLED, 0, VI_FUSION_GRP_SIZE - VI_FUSION_GRP_FILLED);
        arg = fusion;
    }
    return real_ioctl(fd, (int)cmd, arg);
}

static void *map_with_fallback(map_fn cached, map_fn plain,
                               unsigned long long phys, unsigned int size)
{
    void *p = cached != NULL ? cached(phys, size) : NULL;

    if (p == NULL && plain != NULL) p = plain(phys, size);
    return p;
}

void *ss_mpi_sys_mmap_cached(unsigned long long phys, unsigned int size)
{
    (void)pthread_once(&resolved, resolve);
    return map_with_fallback(ss_mmap_cached, ss_mmap, phys, size);
}

void *ot_mpi_sys_mmap_cached(unsigned long long phys, unsigned int size)
{
    (void)pthread_once(&resolved, resolve);
    return map_with_fallback(ot_mmap_cached, ot_mmap, phys, size);
}
