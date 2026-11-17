// Model-bound AppleSMC LED control with optional startup persistence.
#define _DARWIN_C_SOURCE
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <libproc.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

// AppleSMC external method 2: 80-byte request/reply ABI.
// Unused version/power-limit fields and all padding remain zero.
typedef struct {
    uint32_t key;
    uint8_t reserved[24];
    uint32_t size;
    uint32_t type;
    uint8_t attributes;
    uint8_t info_padding[3];
    uint8_t result;
    uint8_t status;
    uint8_t command;
    uint8_t padding;
    uint32_t index;
    uint8_t bytes[32];
} SmcMessage;

_Static_assert(sizeof(SmcMessage) == 80, "SMC message size");
_Static_assert(offsetof(SmcMessage, size) == 28, "SMC info offset");
_Static_assert(offsetof(SmcMessage, result) == 40, "SMC result offset");
_Static_assert(offsetof(SmcMessage, command) == 42, "SMC command offset");
_Static_assert(offsetof(SmcMessage, bytes) == 48, "SMC payload offset");

static const uint32_t led_key = UINT32_C(0x4c533043); // LS0C, not LSOC
static const char supported_model[] = "Mac18,5";
static char detected_model[64];
static const uint32_t led_type = UINT32_C(0x75693136); // ui16

static const char installed_binary[] = "/usr/local/bin/ledctl";
static const char installed_readme[] = "/usr/local/share/doc/ledctl/README.txt";
static const char daemon_plist[] = "/Library/LaunchDaemons/local.ledctl.plist";
static const char managed_marker[] = "<!-- Managed by ledctl. -->";

static int exchange(io_connect_t connection, const SmcMessage *request,
                    SmcMessage *reply) {
    memset(reply, 0, sizeof(*reply));
    size_t size = sizeof(*reply);
    kern_return_t result = IOConnectCallStructMethod(connection, 2, request,
        sizeof(*request), reply, &size);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "AppleSMC command %u: IOKit 0x%08x (%s)\n",
            request->command, (unsigned)result, mach_error_string(result));
        return -1;
    }
    if (size != sizeof(*reply) || reply->result != 0) {
        fprintf(stderr, "AppleSMC command %u: reply size %zu, SMC result 0x%02x, status 0x%02x\n",
            request->command, size, reply->result, reply->status);
        return -1;
    }
    return 0;
}

static int check_model(const char *model, int force_model) {
    if (strcmp(model, supported_model) == 0) return 0;
    if (!force_model) {
        fprintf(stderr, "Unsupported model %s. Verified model: %s.\n"
            "Use --force-model only if you accept the unverified LED mapping.\n",
            model, supported_model);
        return -1;
    }
    fprintf(stderr, "WARNING: unverified model %s; using LS0C at your explicit request.\n",
        model);
    return 0;
}

static int open_smc(io_connect_t *connection, int force_model) {
    size_t size = sizeof(detected_model);
    if (sysctlbyname("hw.model", detected_model, &size, NULL, 0) != 0) {
        perror("hw.model");
        return -1;
    }
    if (check_model(detected_model, force_model) != 0) return -1;
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault,
        IOServiceMatching("AppleSMC"));
    if (service == IO_OBJECT_NULL) {
        fputs("AppleSMC service not found.\n", stderr);
        return -1;
    }
    kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, connection);
    IOObjectRelease(service);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen: 0x%08x (%s)\n", (unsigned)result,
            mach_error_string(result));
        return -1;
    }
    return 0;
}

static int read_led(io_connect_t connection, SmcMessage *info, uint8_t value[2]) {
    SmcMessage request = {0}, reply;
    request.key = led_key;
    request.command = 9; // Read key metadata.
    if (exchange(connection, &request, info) != 0) return -1;
    if (info->size != 2 || info->type != led_type || info->attributes != 0xc4) {
        fprintf(stderr, "Unexpected LS0C metadata: size=%u type=0x%08x attributes=0x%02x; refusing access.\n",
            info->size, info->type, info->attributes);
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.key = led_key;
    request.command = 5; // Read bytes.
    request.size = info->size;
    if (exchange(connection, &request, &reply) != 0) return -1;
    value[0] = reply.bytes[0];
    value[1] = reply.bytes[1];
    return 0;
}

static void print_status(const uint8_t value[2]) {
    printf("model=%s key=LS0C type=ui16 size=2 attributes=0xc4 raw=%02x%02x\n",
        detected_model, value[0], value[1]);
    if (value[0] == value[1])
        printf("state=%s brightness=%.2f%% level=%u/255\n",
            value[0] == 0 ? "off" : "on", value[0] * 100.0 / 255.0, value[0]);
    else
        puts("state=unknown (payload is not the verified vv encoding)");
}

// Explicit manual snapshots are exclusive, private, and flushed to disk.
static int save_snapshot(int fd, const char *path, const uint8_t value[2]) {
    char text[128];
    int length = snprintf(text, sizeof(text),
        "ledctl-v1 %s LS0C ui16 2 %02x %02x\n", detected_model, value[0], value[1]);
    int failed = length < 0 || (size_t)length >= sizeof(text);
    size_t used = 0;
    while (!failed && used < (size_t)length) {
        ssize_t count = write(fd, text + used, (size_t)length - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) failed = 1;
        else used += (size_t)count;
    }
    if (fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (failed) {
        fprintf(stderr, "Could not persist snapshot %s; no hardware write attempted.\n", path);
        return -1;
    }
    fprintf(stderr, "Snapshot: %s (restore with: ledctl restore '%s')\n", path, path);
    return 0;
}

static int snapshot_file(const char *path, const uint8_t value[2]) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    return save_snapshot(fd, path, value);
}

static int load_snapshot(const char *path, uint8_t *level) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 127) {
        fputs("Snapshot must be a small regular file, not a symlink or device.\n", stderr);
        close(fd);
        return -1;
    }
    char text[128] = {0};
    size_t used = 0;
    while (used < sizeof(text) - 1) {
        ssize_t count = read(fd, text + used, sizeof(text) - 1 - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            perror("read snapshot");
            close(fd);
            return -1;
        }
        if (count == 0) break;
        used += (size_t)count;
    }
    close(fd);
    unsigned first = 0, second = 0;
    char model[64];
    int end = -1;
    if (sscanf(text, "ledctl-v1 %63s LS0C ui16 2 %2x %2x%n",
               model, &first, &second, &end) != 3 ||
        end < 0 || (size_t)end + 1 != used || text[end] != '\n' ||
        first > 255 || first != second || strcmp(model, detected_model) != 0) {
        fputs("Snapshot does not match this model/key/type/vv encoding.\n", stderr);
        return -1;
    }
    *level = (uint8_t)first;
    return 0;
}

static int apply_level(io_connect_t connection, uint8_t level) {
    SmcMessage request = {0}, reply, info;
    request.key = led_key;
    request.command = 6; // The ONLY hardware write: LS0C, two equal bytes.
    request.size = 2;
    request.bytes[0] = level;
    request.bytes[1] = level;
    if (exchange(connection, &request, &reply) != 0) return -1;
    uint8_t actual[2];
    if (read_led(connection, &info, actual) != 0) return -1;
    if (actual[0] != level || actual[1] != level) {
        fprintf(stderr, "Readback mismatch: wanted %02x%02x, got %02x%02x.\n",
            level, level, actual[0], actual[1]);
        return -1;
    }
    return 0;
}

static int change_led(io_connect_t connection, const uint8_t before[2], uint8_t level) {
    if (before[0] != before[1]) {
        fputs("Current payload is not verified vv encoding; refusing to write.\n", stderr);
        return -1;
    }
    // Refuse a write if another controller changed the initial reading.
    SmcMessage info;
    uint8_t current[2];
    if (read_led(connection, &info, current) != 0) return -1;
    if (memcmp(current, before, 2) != 0) {
        fputs("LED changed concurrently; no hardware write attempted. Quit other controllers.\n", stderr);
        return -1;
    }
    if (apply_level(connection, level) != 0) {
        fputs("Write/readback failed; attempting to restore the previous state.\n", stderr);
        if (apply_level(connection, before[0]) != 0)
            fputs("RESTORE FAILED. The LED state may have changed.\n", stderr);
        else
            fputs("Previous state restored and verified.\n", stderr);
        return -1;
    }
    const uint8_t value[2] = {level, level};
    print_status(value);
    return 0;
}

static int percent_level(const char *text, uint8_t *level) {
    if (*text == '\0') return -1;
    for (const char *p = text; *p != '\0'; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    unsigned long percent = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || percent > 100) return -1;
    *level = (uint8_t)((percent * 255 + 50) / 100);
    return 0;
}

// Persistence is installation only: launchd runs the normal CLI once at boot.
static int require_root(void) {
    if (getuid() == 0 && geteuid() == 0) return 0;
    fputs("Administrator privileges required. Run this command again with sudo.\n", stderr);
    return -1;
}

static int secure_directory(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        perror(path);
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        fprintf(stderr, "Directory must be root-owned, non-symlink, and not group/other-writable: %s\n", path);
        return -1;
    }
    return 0;
}

static int secure_file(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        perror(path);
        return -1;
    }
    if (!S_ISREG(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022) != 0) {
        fprintf(stderr, "Refusing unsafe installation target: %s\n", path);
        return -1;
    }
    return 0;
}

static int run_command(char *const arguments[], int quiet) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); return -1; }
    if (child == 0) {
        if (quiet) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0)
                _exit(126);
            close(fd);
        }
        execv(arguments[0], arguments);
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno == EINTR) continue;
        perror("waitpid");
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int unload_job(void) {
    char *probe[] = {"/bin/launchctl", "print", "system/local.ledctl", NULL};
    int result = run_command(probe, 1);
    if (result < 0 || result == 126 || result == 127) return -1;
    if (result != 0) return 0; // Job is not loaded.
    char *unload[] = {"/bin/launchctl", "bootout", "system/local.ledctl", NULL};
    if (run_command(unload, 0) == 0) return 0;
    fputs("Could not unload the previous launchd job.\n", stderr);
    return -1;
}

// Do not replace/remove another application's configuration under our label.
static int managed_configuration(void) {
    if (secure_file(daemon_plist) != 0) return -1;
    int fd = open(daemon_plist, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return 0;
        perror(daemon_plist);
        return -1;
    }
    char text[256] = {0};
    ssize_t count;
    do { count = read(fd, text, sizeof(text) - 1); } while (count < 0 && errno == EINTR);
    close(fd);
    if (count > 0 && strstr(text, managed_marker) != NULL) return 1;
    fputs("Existing LaunchDaemon is not managed by ledctl; refusing to replace it.\n", stderr);
    return -1;
}

static int write_installed_text(const char *path, const char *text) {
    if (secure_file(path) != 0) return -1;
    char temporary[256];
    int length = snprintf(temporary, sizeof(temporary), "%s.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return -1;
    int fd = mkstemp(temporary);
    if (fd < 0) { perror(path); return -1; }
    size_t size = strlen(text), used = 0;
    int failed = 0;
    while (used < size) {
        ssize_t count = write(fd, text + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { failed = 1; break; }
        used += (size_t)count;
    }
    if (fchown(fd, 0, 0) != 0 || fchmod(fd, 0644) != 0 || fsync(fd) != 0) failed = 1;
    if (close(fd) != 0) failed = 1;
    if (!failed && rename(temporary, path) == 0) return 0;
    perror(path);
    unlink(temporary);
    return -1;
}

static int remove_persistence(void) {
    if (require_root() != 0) return -1;
    int managed = managed_configuration();
    if (managed < 0) return -1;
    if (managed == 0) {
        puts("No persistent startup configuration is installed.");
        return 0;
    }
    if (secure_directory("/Library", 0755) != 0 ||
        secure_directory("/Library/LaunchDaemons", 0755) != 0 ||
        unload_job() != 0) return -1;
    if (unlink(daemon_plist) != 0) { perror(daemon_plist); return -1; }
    if (unlink("/private/etc/newsyslog.d/ledctl.conf") != 0 &&
    errno != ENOENT) {
        perror("/private/etc/newsyslog.d/ledctl.conf");
        return -1;
    }
    puts("Startup setting removed. Current brightness, binary, README and logs are unchanged.");
    return 0;
}

static int remove_legacy_installation(void) {
    struct stat st;
    if (lstat("/usr/local/libexec", &st) != 0) {
        if (errno == ENOENT) return 0;
        perror("/usr/local/libexec");
        return -1;
    }
    if (secure_directory("/usr/local/libexec", 0755) != 0) return -1;
    const char *files[] = {"/usr/local/libexec/ledctl", "/usr/local/libexec/ledctl.README.txt"};
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i)
        if (secure_file(files[i]) != 0) return -1;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        if (unlink(files[i]) != 0 && errno != ENOENT) {
            perror(files[i]);
            return -1;
        }
    }
    return 0;
}

static int migrate_startup_binary(void) {
    int managed = managed_configuration();
    if (managed < 0) return -1;
    if (managed == 0) return 0;
    if (secure_directory("/Library", 0755) != 0 ||
        secure_directory("/Library/LaunchDaemons", 0755) != 0) return -1;
    int fd = open(daemon_plist, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) { perror(daemon_plist); return -1; }
    char text[4096];
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || st.st_size >= (off_t)sizeof(text)) {
        fputs("Startup configuration is too large or unreadable.\n", stderr);
        close(fd);
        return -1;
    }
    ssize_t count;
    do { count = read(fd, text, (size_t)st.st_size); } while (count < 0 && errno == EINTR);
    close(fd);
    if (count != st.st_size) {
        fputs("Could not read the complete startup configuration.\n", stderr);
        return -1;
    }
    text[count] = '\0';
    // Replace only the first argument in our generated XML. Preserve all other
    // fields and the ownership comment, which generic plist serializers remove.
    char *value = strstr(text, "<key>ProgramArguments</key>");
    if (value != NULL) {
        value += strlen("<key>ProgramArguments</key>");
        value += strspn(value, " \t\r\n");
        if (strncmp(value, "<array>", 7) == 0) {
            value += 7;
            value += strspn(value, " \t\r\n");
            if (strncmp(value, "<string>", 8) == 0) {
                value += 8;
                char *end = strstr(value, "</string>");
                const char old_binary[] = "/usr/local/libexec/ledctl";
                if (end != NULL) {
                    size_t size = (size_t)(end - value);
                    int current = size == strlen(installed_binary) &&
                        memcmp(value, installed_binary, size) == 0;
                    int legacy = size == sizeof(old_binary) - 1 &&
                        memcmp(value, old_binary, size) == 0;
                    if (current || legacy) {
                        if (unload_job() != 0) return -1;
                        if (current) return 0;
                        char updated[4096];
                        int length = snprintf(updated, sizeof(updated), "%.*s%s%s",
                            (int)(value - text), text, installed_binary, end);
                        if (length >= 0 && (size_t)length < sizeof(updated))
                            return write_installed_text(daemon_plist, updated);
                    }
                }
            }
        }
    }
    fputs("Unrecognized startup command; refusing to replace it.\n", stderr);
    return -1;
}

static int install_cli(void) {
    if (require_root() != 0) return -1;
    const char *directories[] = {"/usr", "/usr/local", "/usr/local/bin",
        "/usr/local/share", "/usr/local/share/doc", "/usr/local/share/doc/ledctl"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        if (secure_directory(directories[i], 0755) != 0) return -1;
    if (secure_file(installed_binary) != 0) return -1;

    char executable[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), executable, sizeof(executable)) <= 0) {
        perror("proc_pidpath");
        return -1;
    }
    struct stat source, target;
    int same_file = stat(executable, &source) == 0 && stat(installed_binary, &target) == 0 &&
        source.st_dev == target.st_dev && source.st_ino == target.st_ino;
    if (!same_file) {
        char *copy[] = {"/usr/bin/install", "-S", "-o", "root", "-g", "wheel",
            "-m", "755", executable, (char *)installed_binary, NULL};
        if (run_command(copy, 0) != 0) {
            fputs("Could not install the root-owned CLI.\n", stderr);
            return -1;
        }
    }
    static const char readme[] =
        "ledctl - direct AppleSMC LED control\n\n"
        "sudo ledctl 20                Set 20% now.\n"
        "sudo ledctl 20 --persistent   Also save 20% for boot.\n"
        "sudo ledctl --unpersist       Remove the startup setting.\n"
        "ledctl status                Read actual hardware state.\n\n"
        "Installed CLI: /usr/local/bin/ledctl (independent of the build folder).\n"
        "Optional backup: ledctl snapshot FILE\n"
        "Restore: sudo ledctl restore FILE\n"
        "Writes are verified; errors trigger one in-memory restoration attempt.\n"
        "No automatic snapshot files are created.\n"
        "Startup logs: /var/db/ledctl (root-only).\n"
        "LaunchDaemon: /Library/LaunchDaemons/local.ledctl.plist\n"
        "The saved value is applied once at boot, not continuously or on wake.\n"
        "A command without --persistent does not change the saved startup value.\n"
        "No resident process, setuid helper, or password file is needed.\n"
        "Verified model: Mac18,5. Other models require --force-model.\n"
        "WARNING: --force-model does not verify the LED mapping on another model.\n"
        "With --persistent, --force-model is also saved for boot.\n"
        "Quit other LED controllers.\n";
    return write_installed_text(installed_readme, readme);
}

static int install_persistence(uint8_t level, int force_model) {
    if (install_cli() != 0) return -1;
    const char *directories[] = {"/Library", "/Library/LaunchDaemons",
        "/private", "/private/var", "/private/var/db"};
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i)
        if (secure_directory(directories[i], 0755) != 0) return -1;
    if (secure_directory("/private/var/db/ledctl", 0700) != 0 ||
        secure_file("/private/var/db/ledctl/stdout.log") != 0 ||
        secure_file("/private/var/db/ledctl/stderr.log") != 0 ||
        managed_configuration() < 0) return -1;
    
    if (secure_directory("/private/etc/newsyslog.d", 0755) != 0)
        return -1;

    static const char log_rotation[] =
        "/var/db/ledctl/stdout.log root:wheel 600 2 64 * N\n"
        "/var/db/ledctl/stderr.log root:wheel 600 2 64 * N\n";

    if (write_installed_text(
            "/private/etc/newsyslog.d/ledctl.conf",
            log_rotation) != 0)
        return -1;

    unsigned percent = (level * 100u + 127u) / 255u;
    char plist[2048];
    int length = snprintf(plist, sizeof(plist),
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n%s\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\"><dict>\n"
        "<key>Label</key><string>local.ledctl</string>\n"
        "<key>ProgramArguments</key><array><string>%s</string><string>%u</string>%s</array>\n"
        "<key>UserName</key><string>root</string>\n"
        "<key>WorkingDirectory</key><string>/var/db/ledctl</string>\n"
        "<key>RunAtLoad</key><true/>\n"
        "<key>StandardOutPath</key><string>/var/db/ledctl/stdout.log</string>\n"
        "<key>StandardErrorPath</key><string>/var/db/ledctl/stderr.log</string>\n"
        "</dict></plist>\n", managed_marker, installed_binary, percent,
        force_model ? "<string>--force-model</string>" : "");
    if (length < 0 || (size_t)length >= sizeof(plist) || unload_job() != 0 ||
        write_installed_text(daemon_plist, plist) != 0 ||
        remove_legacy_installation() != 0) return -1;
    printf("Startup brightness saved: %u%%. Applies at the next system boot.\n"
           "Installed CLI: %s\nInstalled README: %s\n"
           "No background process started. Disable with: sudo %s --unpersist\n",
           percent, installed_binary, installed_readme, installed_binary);
    return 0;
}

static void usage(FILE *out) {
    fputs("Usage: ledctl 0..100 | off | on | brightness 0..100 [--persistent]\n"
          "       ledctl status | snapshot FILE | restore FILE | --install | --unpersist\n"
          "Other models require --force-model; their LED mapping is unverified.\n"
          "--install copies the CLI into /usr/local/bin without changing brightness.\n"
          "--persistent sets brightness now and installs that value for system boot.\n"
          "--unpersist removes the startup setting without changing brightness.\n"
          "Writes require sudo. No automatic snapshot files are created.\n", out);
}

int main(int argc, char **argv) {
    int persistent = 0, force_model = 0, count = 1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--persistent") == 0) {
            if (persistent) { usage(stderr); return 2; }
            persistent = 1;
        } else if (strcmp(argv[i], "--force-model") == 0) {
            if (force_model) { usage(stderr); return 2; }
            force_model = 1;
        } else {
            argv[count++] = argv[i];
        }
    }
    argc = count;
    argv[count] = NULL;
    if (argc == 2 && strcmp(argv[1], "--install") == 0) {
        if (persistent || force_model) { usage(stderr); return 2; }
        if (install_cli() != 0 || migrate_startup_binary() != 0 ||
            remove_legacy_installation() != 0) return 1;
        printf("Installed CLI: %s\nInstalled README: %s\n"
               "Run ledctl from any directory. Brightness is unchanged.\n",
               installed_binary, installed_readme);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--unpersist") == 0) {
        if (persistent || force_model) { usage(stderr); return 2; }
        return remove_persistence() == 0 ? 0 : 1;
    }
    enum { STATUS, SNAPSHOT, CHANGE } action;
    uint8_t level = 0;
    const char *restore_path = NULL;
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0)) {
        usage(stdout);
        return 0;
    } else if (argc == 2 && strcmp(argv[1], "status") == 0) {
        action = STATUS;
    } else if (argc == 2 && percent_level(argv[1], &level) == 0) {
        action = CHANGE;
    } else if (argc == 2 && (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "on") == 0)) {
        action = CHANGE;
        level = strcmp(argv[1], "on") == 0 ? 255 : 0;
    } else if (argc == 3 && strcmp(argv[1], "brightness") == 0 &&
               percent_level(argv[2], &level) == 0) {
        action = CHANGE;
    } else if (argc == 3 && strcmp(argv[1], "snapshot") == 0) {
        action = SNAPSHOT;
    } else if (argc == 3 && strcmp(argv[1], "restore") == 0) {
        if (persistent) { usage(stderr); return 2; }
        restore_path = argv[2];
        action = CHANGE;
    } else {
        usage(stderr);
        return 2;
    }
    if (persistent && action != CHANGE) { usage(stderr); return 2; }
    if (action == CHANGE && require_root() != 0) return 1;
    io_connect_t connection = IO_OBJECT_NULL;
    if (open_smc(&connection, force_model) != 0) return 1;
    SmcMessage info;
    uint8_t value[2];
    int result = restore_path != NULL ? load_snapshot(restore_path, &level) : 0;
    if (result == 0) result = read_led(connection, &info, value);
    if (result == 0) {
        switch (action) {
        case STATUS: print_status(value); break;
        case SNAPSHOT: result = snapshot_file(argv[2], value); break;
        case CHANGE:
            result = change_led(connection, value, level);
            if (result == 0 && persistent && install_persistence(level, force_model) != 0) {
                fputs("LED brightness was set, but persistence setup failed.\n", stderr);
                result = -1;
            }
            break;
        }
    }
    IOServiceClose(connection);
    return result == 0 ? 0 : 1;
}
