/* core/psx/disc_fs_probe.c - see openpete_disc_cache.h (the auto mode's
 * filesystem classifier). The classifiers are pure and compile on every
 * platform so the unit test feeds them values of any OS; the probe calls
 * the host's own query. */
#include "openpete_disc_cache.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#  include <windows.h>
#elif defined(__linux__)
#  include <sys/vfs.h>
#elif defined(__APPLE__)
#  include <sys/param.h>
#  include <sys/mount.h>
#endif

/* Linux statfs f_type values (linux/magic.h and the filesystems' own
 * headers), with the name the boot log prints. slow = 1 marks a filesystem
 * whose reads cross a network or a VM boundary. */
typedef struct {
    uint64_t    magic;
    const char* name;
    int         slow;
} dfs_magic_t;

static const dfs_magic_t k_magic[] = {
    { 0x01021997u, "9p",       1 },   /* V9FS_MAGIC: WSL2 /mnt/c (drvfs), VM shares */
    { 0x00006969u, "nfs",      1 },   /* NFS_SUPER_MAGIC */
    { 0xFF534D42u, "cifs",     1 },   /* CIFS_SUPER_MAGIC */
    { 0x0000517Bu, "smb",      1 },   /* SMB_SUPER_MAGIC */
    { 0xFE534D42u, "smb2",     1 },   /* SMB2_SUPER_MAGIC */
    { 0x65735546u, "fuse",     1 },   /* FUSE_SUPER_MAGIC: sshfs, virtiofs, ntfs-3g */
    { 0x0000EF53u, "ext4",     0 },   /* EXT4_SUPER_MAGIC, shared by ext2 and ext3 */
    { 0x9123683Eu, "btrfs",    0 },
    { 0x58465342u, "xfs",      0 },
    { 0xF2F52010u, "f2fs",     0 },
    { 0x2FC12FC1u, "zfs",      0 },
    { 0x01021994u, "tmpfs",    0 },
    { 0x794C7630u, "overlayfs",0 },
    { 0x00004D44u, "vfat",     0 },
    { 0x2011BAB0u, "exfat",    0 },
    { 0x7366746Eu, "ntfs3",    0 },
    { 0x00009660u, "iso9660",  0 },
};

int openpete_disc_fs_classify_ftype(uint64_t f_type, char* why, size_t cap) {
    for (size_t i = 0; i < sizeof k_magic / sizeof k_magic[0]; i++) {
        if (k_magic[i].magic != f_type) continue;
        snprintf(why, cap, "%s%s", k_magic[i].slow ? "" : "local ", k_magic[i].name);
        return k_magic[i].slow;
    }
    snprintf(why, cap, "local fs 0x%llx", (unsigned long long)f_type);
    return 0;
}

/** @brief Whether a path is UNC: two leading separators naming a share, or the long form with the UNC prefix; a long-form drive path or a device path is not. */
static int path_is_unc(const char* path) {
    const int sep0 = path[0] == '\\' || path[0] == '/';
    const int sep1 = sep0 && (path[1] == '\\' || path[1] == '/');
    if (!sep1) return 0;
    if ((path[2] == '?' || path[2] == '.') && (path[3] == '\\' || path[3] == '/'))
        return strncmp(path + 4, "UNC", 3) == 0 && (path[7] == '\\' || path[7] == '/');
    return 1;
}

int openpete_disc_fs_classify_windows(unsigned drive_type, const char* path,
                                      char* why, size_t cap) {
    if (path && path_is_unc(path)) {
        const char* host = path + 2;
        if (strncmp(path + 2, "?\\UNC\\", 6) == 0 || strncmp(path + 2, "?/UNC/", 6) == 0) host = path + 8;
        const int wsl = strncmp(host, "wsl$", 4) == 0 || strncmp(host, "wsl.localhost", 13) == 0;
        snprintf(why, cap, "%s", wsl ? "unc wsl" : "unc");
        return 1;
    }
    switch (drive_type) {
    case 4: snprintf(why, cap, "remote drive"); return 1;   /* DRIVE_REMOTE */
    case 5: snprintf(why, cap, "cd-rom drive"); return 1;   /* DRIVE_CDROM */
    case 2: snprintf(why, cap, "local removable drive"); return 0;   /* DRIVE_REMOVABLE */
    case 3: snprintf(why, cap, "local fixed drive"); return 0;       /* DRIVE_FIXED */
    case 6: snprintf(why, cap, "local ram disk"); return 0;          /* DRIVE_RAMDISK */
    default: snprintf(why, cap, "local, drive type %u", drive_type); return 0;
    }
}

#if defined(_WIN32)
/** @brief Convert a path to UTF-16: UTF-8 when it is valid UTF-8, else the ANSI code page fopen reads it in. */
static int path_wide(const char* path, wchar_t* w, int cap) {
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, cap)) return 1;
    return MultiByteToWideChar(CP_ACP, 0, path, -1, w, cap) != 0;
}
#endif

int openpete_disc_fs_slow(const char* path, char* why, size_t cap) {
    if (!path || !*path) { snprintf(why, cap, "no path"); return 0; }
#if defined(_WIN32)
    if (path_is_unc(path)) return openpete_disc_fs_classify_windows(0, path, why, cap);
    wchar_t w[4096], full[4096], root[4096];
    if (!path_wide(path, w, 4096)) { snprintf(why, cap, "path does not convert"); return 0; }
    DWORD n = GetFullPathNameW(w, 4096, full, NULL);
    if (n == 0 || n >= 4096) { snprintf(why, cap, "GetFullPathNameW error %lu", (unsigned long)GetLastError()); return 0; }
    /* A full path that resolved to a UNC share (a relative path under a
     * share's working directory) classifies as one. */
    if (full[0] == L'\\' && full[1] == L'\\' && full[2] != L'?' && full[2] != L'.')
        return openpete_disc_fs_classify_windows(0, "\\\\", why, cap);
    if (!GetVolumePathNameW(full, root, 4096)) {
        snprintf(why, cap, "GetVolumePathNameW error %lu", (unsigned long)GetLastError());
        return 0;
    }
    return openpete_disc_fs_classify_windows(GetDriveTypeW(root), NULL, why, cap);
#elif defined(__linux__)
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) { snprintf(why, cap, "statfs: %s", strerror(errno)); return 0; }
    return openpete_disc_fs_classify_ftype((uint64_t)(unsigned long)sfs.f_type, why, cap);
#elif defined(__APPLE__)
    struct statfs sfs;
    if (statfs(path, &sfs) != 0) { snprintf(why, cap, "statfs: %s", strerror(errno)); return 0; }
    const int slow = (sfs.f_flags & MNT_LOCAL) == 0;
    snprintf(why, cap, "%s%s", slow ? "" : "local ", sfs.f_fstypename);
    return slow;
#else
    snprintf(why, cap, "no filesystem query on this platform");
    return 0;
#endif
}
