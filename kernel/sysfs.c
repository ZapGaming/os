#include <kernel/sysfs.h>
#include <kernel/scheduler.h>
#include <kernel/capability.h>
#include <kernel/ipc.h>
#include <kernel/pit.h>
#include <kernel/pmm.h>
#include <kernel/apic.h>
#include <drivers/ac97.h>
#include <net/net.h>
#include <fs/fat32.h>
#include <string.h>

static int append(char *out, uint32_t cap, int pos, const char *s) {
    if (!out || cap == 0) return 0;
    while (*s && pos < (int)cap - 1) out[pos++] = *s++;
    out[pos] = 0;
    return pos;
}

static int append_u32(char *out, uint32_t cap, int pos, uint32_t v) {
    char tmp[16];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < 15) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = n - 1; i >= 0; i--) {
        if (pos >= (int)cap - 1) break;
        out[pos++] = tmp[i];
    }
    out[pos] = 0;
    return pos;
}

static int append_hex(char *out, uint32_t cap, int pos, uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    pos = append(out, cap, pos, "0x");
    int started = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint32_t d = (v >> shift) & 0xF;
        if (d || started || shift == 0) {
            if (pos >= (int)cap - 1) break;
            out[pos++] = hex[d];
            started = 1;
        }
    }
    out[pos] = 0;
    return pos;
}

static int append_ip(char *out, uint32_t cap, int pos, uint32_t ip) {
    for (int i = 0; i < 4; i++) {
        pos = append_u32(out, cap, pos, (ip >> (24 - i * 8)) & 0xFF);
        if (i != 3) pos = append(out, cap, pos, ".");
    }
    return pos;
}

void sysfs_init(void) {
}

int sysfs_exists(const char *path) {
    if (!path) return 0;
    return strcmp(path, "/") == 0 || strcmp(path, "/proc") == 0 ||
           strcmp(path, "/sys") == 0 || strcmp(path, "/dev") == 0 ||
           strcmp(path, "/proc/tasks") == 0 || strcmp(path, "/proc/ipc") == 0 ||
           strcmp(path, "/sys/kernel") == 0 || strcmp(path, "/sys/memory") == 0 ||
           strcmp(path, "/sys/network") == 0 || strcmp(path, "/sys/audio") == 0 ||
           strcmp(path, "/sys/storage") == 0 || strcmp(path, "/dev/null") == 0 ||
           strcmp(path, "/dev/console") == 0 || strncmp(path, "/proc/", 6) == 0;
}

int sysfs_list(const char *path, char *out, uint32_t cap) {
    if (!out || cap == 0 || !path) return 0;
    out[0] = 0;
    if (strcmp(path, "/") == 0) return append(out, cap, 0, "proc/\nsys/\ndev/\n");
    if (strcmp(path, "/proc") == 0) {
        int pos = append(out, cap, 0, "tasks\nipc\n");
        for (int i = 0; i < scheduler_task_count(); i++) {
            pos = append_u32(out, cap, pos, (uint32_t)i);
            pos = append(out, cap, pos, "/\n");
        }
        return pos;
    }
    if (strcmp(path, "/sys") == 0) return append(out, cap, 0, "kernel\nmemory\nnetwork\naudio\nstorage\n");
    if (strcmp(path, "/dev") == 0) return append(out, cap, 0, "null\nconsole\n");
    return 0;
}

int sysfs_read(const char *path, char *out, uint32_t cap) {
    if (!out || cap == 0 || !path) return 0;
    out[0] = 0;
    int pos = 0;

    if (strcmp(path, "/proc/tasks") == 0) {
        for (int i = 0; i < scheduler_task_count(); i++) {
            struct scheduler_task_info t;
            if (!scheduler_task_snapshot(i, &t)) continue;
            pos = append_u32(out, cap, pos, (uint32_t)t.pid);
            pos = append(out, cap, pos, " ");
            pos = append(out, cap, pos, t.name);
            pos = append(out, cap, pos, " state=");
            pos = append(out, cap, pos, t.state == TASK_RUNNING ? "running" : t.state == TASK_READY ? "ready" : "terminated");
            pos = append(out, cap, pos, " cpu=");
            pos = append_u32(out, cap, pos, (uint32_t)t.cpu_affinity);
            pos = append(out, cap, pos, " runtime=");
            pos = append_u32(out, cap, pos, t.runtime_ticks);
            pos = append(out, cap, pos, " switches=");
            pos = append_u32(out, cap, pos, t.switches);
            pos = append(out, cap, pos, "\n");
        }
        return pos;
    }

    if (strcmp(path, "/proc/ipc") == 0) {
        pos = append(out, cap, pos, "messages=");
        pos = append_u32(out, cap, pos, ipc_total_messages());
        pos = append(out, cap, pos, " dropped=");
        pos = append_u32(out, cap, pos, ipc_total_dropped());
        pos = append(out, cap, pos, " endpoints=");
        pos = append_u32(out, cap, pos, (uint32_t)ipc_endpoint_count());
        pos = append(out, cap, pos, "\n");
        for (int i = 0; i < ipc_endpoint_count(); i++) {
            struct ipc_endpoint_info ep;
            if (!ipc_endpoint_snapshot(i, &ep)) continue;
            pos = append(out, cap, pos, ep.service[0] ? ep.service : "anonymous");
            pos = append(out, cap, pos, " pid=");
            pos = append_u32(out, cap, pos, (uint32_t)ep.pid);
            pos = append(out, cap, pos, " queued=");
            pos = append_u32(out, cap, pos, (uint32_t)ep.queued);
            pos = append(out, cap, pos, " sent=");
            pos = append_u32(out, cap, pos, ep.sent);
            pos = append(out, cap, pos, " received=");
            pos = append_u32(out, cap, pos, ep.received);
            pos = append(out, cap, pos, "\n");
        }
        return pos;
    }

    if (strncmp(path, "/proc/", 6) == 0) {
        const char *p = path + 6;
        uint32_t pid = 0;
        while (*p >= '0' && *p <= '9') { pid = pid * 10 + (uint32_t)(*p - '0'); p++; }
        struct scheduler_task_info t;
        if (!scheduler_task_snapshot((int)pid, &t)) return 0;
        pos = append(out, cap, pos, "name="); pos = append(out, cap, pos, t.name);
        pos = append(out, cap, pos, "\nstate="); pos = append(out, cap, pos, t.state == TASK_RUNNING ? "running" : t.state == TASK_READY ? "ready" : "terminated");
        pos = append(out, cap, pos, "\nkind="); pos = append_u32(out, cap, pos, (uint32_t)t.kind);
        pos = append(out, cap, pos, "\ncpu="); pos = append_u32(out, cap, pos, (uint32_t)t.cpu_affinity);
        pos = append(out, cap, pos, "\nruntime_ticks="); pos = append_u32(out, cap, pos, t.runtime_ticks);
        pos = append(out, cap, pos, "\nswitches="); pos = append_u32(out, cap, pos, t.switches);
        pos = append(out, cap, pos, "\ncapabilities="); pos = append_hex(out, cap, pos, capability_get((int)pid));
        pos = append(out, cap, pos, "\n");
        return pos;
    }

    if (strcmp(path, "/sys/kernel") == 0) {
        pos = append(out, cap, pos, "name=ZapOS Nova\narchitecture=i686\nuptime_ticks=");
        pos = append_u32(out, cap, pos, pit_ticks());
        pos = append(out, cap, pos, "\ncpus="); pos = append_u32(out, cap, pos, apic_ap_started_count() ? 2 : 1);
        pos = append(out, cap, pos, "\ntasks="); pos = append_u32(out, cap, pos, (uint32_t)scheduler_task_count());
        pos = append(out, cap, pos, "\ncontext_switches="); pos = append_u32(out, cap, pos, scheduler_context_switches());
        pos = append(out, cap, pos, "\n");
        return pos;
    }

    if (strcmp(path, "/sys/memory") == 0) {
        pos = append(out, cap, pos, "frame_size=4096\nfree_frames=");
        pos = append_u32(out, cap, pos, pmm_free_frame_count());
        pos = append(out, cap, pos, "\nfree_bytes=");
        pos = append_u32(out, cap, pos, pmm_free_frame_count() * PMM_FRAME_SIZE);
        pos = append(out, cap, pos, "\n");
        return pos;
    }

    if (strcmp(path, "/sys/network") == 0) {
        pos = append(out, cap, pos, "state="); pos = append(out, cap, pos, net_is_up() ? "up" : "down");
        pos = append(out, cap, pos, "\ndriver="); pos = append(out, cap, pos, net_get_driver_name());
        pos = append(out, cap, pos, "\nip="); pos = append_ip(out, cap, pos, net_get_ip());
        pos = append(out, cap, pos, "\ngateway="); pos = append_ip(out, cap, pos, net_get_gateway_ip());
        pos = append(out, cap, pos, "\n");
        return pos;
    }

    if (strcmp(path, "/sys/audio") == 0) {
        pos = append(out, cap, pos, "device="); pos = append(out, cap, pos, ac97_is_present() ? "ac97" : "none");
        pos = append(out, cap, pos, "\nplaying="); pos = append(out, cap, pos, ac97_is_playing() ? "yes" : "no");
        pos = append(out, cap, pos, "\n");
        return pos;
    }

    if (strcmp(path, "/sys/storage") == 0) {
        pos = append(out, cap, pos, "fat32="); pos = append(out, cap, pos, fat32_is_mounted() ? "mounted" : "unavailable");
        pos = append(out, cap, pos, "\nroot_cluster="); pos = append_u32(out, cap, pos, fat32_is_mounted() ? fat32_root_cluster() : 0);
        pos = append(out, cap, pos, "\n");
        return pos;
    }

    if (strcmp(path, "/dev/null") == 0) return 0;
    if (strcmp(path, "/dev/console") == 0) return append(out, cap, 0, "ZapOS Nova console\n");
    return 0;
}
