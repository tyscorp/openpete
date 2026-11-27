/* tests/unit/disc_fs_probe_test.c - the --disc-cache auto classifier on
 * injected values: Linux statfs f_type magics (9p, NFS, CIFS, SMB, SMB2,
 * FUSE slow; ext4, btrfs, tmpfs and an unlisted value local), Windows drive
 * types (remote and CD-ROM slow; fixed, removable, ram disk and unknown
 * local) and UNC paths (\\server\share, \\wsl$, \\wsl.localhost and
 * \\?\UNC\ slow whatever the drive type; \\?\C:\ and \\.\ not UNC), and
 * the probe on this host's temporary directory, which returns a reason. */
#include "openpete_disc_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_fail;

/** @brief Check one f_type: the verdict and the reason text. */
static void ftype_case(uint64_t f_type, int slow, const char* why_want) {
    char why[64];
    const int got = openpete_disc_fs_classify_ftype(f_type, why, sizeof why);
    if (got != slow || strcmp(why, why_want) != 0) {
        printf("FAIL f_type 0x%llx: %d (%s), want %d (%s)\n", (unsigned long long)f_type,
               got, why, slow, why_want);
        s_fail++;
    }
}

/** @brief Check one Windows drive type and path: the verdict and the reason text. */
static void win_case(unsigned drive_type, const char* path, int slow, const char* why_want) {
    char why[64];
    const int got = openpete_disc_fs_classify_windows(drive_type, path, why, sizeof why);
    if (got != slow || strcmp(why, why_want) != 0) {
        printf("FAIL drive %u path %s: %d (%s), want %d (%s)\n", drive_type,
               path ? path : "(null)", got, why, slow, why_want);
        s_fail++;
    }
}

/** @brief Run the classifier cases and the host probe; exit 1 on any failure. */
int main(void) {
    ftype_case(0x01021997u, 1, "9p");
    ftype_case(0x6969u, 1, "nfs");
    ftype_case(0xFF534D42u, 1, "cifs");
    ftype_case(0x517Bu, 1, "smb");
    ftype_case(0xFE534D42u, 1, "smb2");
    ftype_case(0x65735546u, 1, "fuse");
    ftype_case(0xEF53u, 0, "local ext4");
    ftype_case(0x9123683Eu, 0, "local btrfs");
    ftype_case(0x01021994u, 0, "local tmpfs");
    ftype_case(0x12345678u, 0, "local fs 0x12345678");

    win_case(4, "Z:\\games\\spyro.cue", 1, "remote drive");
    win_case(5, "D:\\SCUS_942.28", 1, "cd-rom drive");
    win_case(3, "C:\\games\\spyro.bin", 0, "local fixed drive");
    win_case(2, "E:\\spyro.bin", 0, "local removable drive");
    win_case(6, NULL, 0, "local ram disk");
    win_case(1, NULL, 0, "local, drive type 1");
    win_case(3, "\\\\nas\\games\\spyro.bin", 1, "unc");
    win_case(3, "//nas/games/spyro.bin", 1, "unc");
    win_case(3, "\\\\wsl$\\Ubuntu\\home\\spyro.bin", 1, "unc wsl");
    win_case(3, "\\\\wsl.localhost\\Ubuntu\\spyro.bin", 1, "unc wsl");
    win_case(3, "\\\\?\\UNC\\nas\\games\\spyro.bin", 1, "unc");
    win_case(3, "\\\\?\\C:\\games\\spyro.bin", 0, "local fixed drive");
    win_case(3, "\\\\.\\C:\\games\\spyro.bin", 0, "local fixed drive");

    char why[96];
    const char* tmp = getenv("TMPDIR");
    const int slow = openpete_disc_fs_slow(tmp && *tmp ? tmp : "/tmp", why, sizeof why);
    printf("disc_fs_probe_test: host temp dir: %s (%s)\n", slow ? "slow" : "local", why);
    if (!why[0]) { printf("FAIL host probe gave no reason\n"); s_fail++; }
    why[0] = 'x';
    if (openpete_disc_fs_slow("", why, sizeof why) != 0 || strcmp(why, "no path") != 0) {
        printf("FAIL empty path: %s\n", why); s_fail++;
    }
    if (s_fail) return 1;
    printf("disc_fs_probe_test: OK\n");
    return 0;
}
