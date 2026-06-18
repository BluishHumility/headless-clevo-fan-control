/*
 ============================================================================
 Name        : clevo-indicator.c
 Author      : AqD <iiiaqd@gmail.com>
 Version     :
 Description : Ubuntu fan control indicator for Clevo laptops

 Based on http://www.association-apml.fr/upload/fanctrl.c by Jonas Diemer
 (diemer@gmx.de)

 ============================================================================

 TEST:
 gcc clevo-indicator.c -o clevo-indicator `pkg-config --cflags --libs appindicator3-0.1` -lm
 sudo chown root clevo-indicator
 sudo chmod u+s clevo-indicator

 Run as effective uid = root, but uid = desktop user (in order to use indicator).

 ============================================================================
 Auto fan control algorithm:

 The algorithm is to replace the builtin auto fan-control algorithm in Clevo
 laptops which is apparently broken in recent models such as W350SSQ, where the
 fan doesn't get kicked until both of GPU and CPU are really hot (and GPU
 cannot be hot anymore thanks to nVIDIA's Maxwell chips). It's far more
 aggressive than the builtin algorithm in order to keep the temperatures below
 60°C all the time, for maximized performance with Intel turbo boost enabled.

 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>



#define NAME "clevo-fan-control"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read by EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_FAN_DUTY 0xCE
#define EC_REG_FAN_RPMS_HI 0xD0
#define EC_REG_FAN_RPMS_LO 0xD1

#define MAX_FAN_RPM 4400.0
#define MIN_FAN_DUTY 16


static void main_init_share(void);

static int main_ec_worker(void);

static int main_dump_fan(void);

static int main_test_fan(int duty_percentage);

static void ec_on_sigterm(int signum);

static int ec_init(void);

static int ec_auto_duty_adjust(void);

static int ec_query_cpu_temp(void);

static int ec_query_gpu_temp(void);

static int ec_query_fan_duty(void);

static int ec_query_fan_rpms(void);

static int ec_write_fan_duty(int duty_percentage);

static int ec_io_wait(const uint32_t port, const uint32_t flag,
                      const char value);

static uint8_t ec_io_read(const uint32_t port);

static int ec_io_do(const uint32_t cmd, const uint32_t port,
                    const uint8_t value);

static int calculate_fan_duty(int raw_duty);

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);

static int check_proc_instances(const char *proc_name);

static void get_time_string(char *buffer, size_t max, const char *format);

static void signal_term(__sighandler_t handler);

static int64_t millis();

static float get_point_along_line(float x0, float y0, float x1, float y1, float xp);

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget *widget;

static struct {
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int fan_duty;
    volatile int fan_rpms;
    volatile int auto_duty;
    volatile int auto_duty_val;
    volatile int manual_next_fan_duty;
    volatile int manual_prev_fan_duty;
    volatile int64_t last_update_time_ms;
    volatile int last_speed_change_delta;
    volatile int64_t last_speed_change_direction_time_ms;
} state;

static pid_t parent_pid = 0;

int main(int argc, char *argv[])
{
    printf("Clevo Fan Control (headless)\n");

    // Optional: kill switch
    if (argc > 1 && strcmp(argv[1], "exit") == 0) {
        printf("Killing all instances...\n");
        char killCommand[256];
        snprintf(killCommand, sizeof(killCommand),
                 "pkill -f %s", NAME);
        system(killCommand);
        return EXIT_SUCCESS;
    }

    // Optional: manual override mode (debug tool)
    if (argc > 1) {
        if (argv[1][0] == '-') {
            printf(
                "Usage:\n"
                "  clevo-fan-control        (automatic daemon)\n"
                "  clevo-fan-control <0-100> (manual fan duty)\n"
                "  clevo-fan-control exit    (stop all instances)\n"
            );
            return main_dump_fan();
        }

        int val = atoi(argv[1]);
        if (val < 0 || val > 100) {
            printf("invalid fan duty %d\n", val);
            return EXIT_FAILURE;
        }

        return main_test_fan(val);
    }

    // Init EC access
    if (ec_init() != EXIT_SUCCESS) {
        fprintf(stderr, "ec_init failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    // No GUI, no fork, no DISPLAY checks, no shared memory
    signal_term(ec_on_sigterm);

    printf("Starting automatic fan control loop...\n");

    return main_ec_worker();
}

static void main_init_share(void) {
    void *shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
                     -1, 0);
    share_info = shm;
    state.exit = 0;
    state.cpu_temp = 0;
    state.gpu_temp = 0;
    state.fan_duty = -1;
    state.fan_rpms = 0;
    state.auto_duty = 1;
    state.auto_duty_val = -1;
    state.manual_next_fan_duty = 0;
    state.manual_prev_fan_duty = 0;
    state.last_update_time_ms = millis();
    state.last_speed_change_delta = 0;
    state.last_speed_change_direction_time_ms = 0;
}

static int main_ec_worker(void) {
    setuid(0);
    int ret = system("/usr/sbin/modinfo ec_sys > /dev/null");
    char *ec_path;
    if (ret==0) {
        system("/usr/sbin/modprobe ec_sys");
        ec_path = "/sys/kernel/debug/ec/ec0/io";
        ret=1;
    } else {
        printf("no module ec_sys, trying acpi_ec\n");
        ret = system("/usr/sbin/modinfo acpi_ec > /dev/null");
        if (ret==0) {
            system("/usr/sbin/modprobe acpi_ec");
            ec_path = "/dev/ec";
            ret=2;
        } else {
            printf("no acpi_ec module, try running a debug kernel with ec_sys or install https://github.com/musikid/acpi_ec.git\n");
            return EXIT_FAILURE;
        }
    }

    while (state.exit == 0) {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
        // write EC
        int new_fan_duty = state.manual_next_fan_duty;
        if (new_fan_duty != state.manual_prev_fan_duty) {
            ec_write_fan_duty(new_fan_duty);
            state.manual_prev_fan_duty = new_fan_duty;
        }
        // read EC
        int io_fd = open(ec_path, O_RDONLY, 0);
        if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len) {
            case -1:
                printf("unable to read EC from sysfs: %s\n", strerror(errno));
                break;
            case 0x100:
                state.cpu_temp = buf[EC_REG_CPU_TEMP];
                state.gpu_temp = buf[EC_REG_GPU_TEMP];
                state.fan_duty = calculate_fan_duty(buf[EC_REG_FAN_DUTY]);
                state.fan_rpms = calculate_fan_rpms(buf[EC_REG_FAN_RPMS_HI],
                                                          buf[EC_REG_FAN_RPMS_LO]);
                /*
                 printf("temp=%d, duty=%d, rpms=%d\n", state.cpu_temp,
                 state.fan_duty, state.fan_rpms);
                 */
                break;
            default:
                printf("wrong EC size from sysfs: %ld\n", len);
        }
        close(io_fd);
        // auto EC
        if (state.auto_duty == 1) {
            int next_duty = ec_auto_duty_adjust();
            if (next_duty != state.auto_duty_val) {
//                char s_time[256];
//                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
//                printf("%s CPU=%d°C, GPU=%d°C, auto fan duty to %d%%\n", s_time,
//                       state.cpu_temp, state.gpu_temp, next_duty);
                ec_write_fan_duty(next_duty);
                state.auto_duty_val = next_duty;
            }
        }
        //
        usleep(200 * 1000);
    }
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static int main_dump_fan(void) {
    printf("Dump fan information\n");
    printf("  FAN Duty: %d%%\n", ec_query_fan_duty());
    printf("  FAN RPMs: %d RPM\n", ec_query_fan_rpms());
    printf("  CPU Temp: %d°C\n", ec_query_cpu_temp());
    printf("  GPU Temp: %d°C\n", ec_query_gpu_temp());
    return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage) {
    printf("Change fan duty to %d%%\n", duty_percentage);
    ec_write_fan_duty(duty_percentage);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static int ec_init(void) {
    if (ioperm(EC_DATA, 1, 1) != 0)
        return EXIT_FAILURE;
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum) {
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        state.exit = 1;
}

static int ec_auto_duty_adjust(void) {
    int temp = MAX(state.cpu_temp, state.gpu_temp);
    int last_fan_duty = MAX(MIN_FAN_DUTY, state.auto_duty_val);
    int min_time_until_next_update_ms = 333;

    // Determine time difference since last update.
    int64_t now = millis();
    int64_t diff_t = now - state.last_update_time_ms;

    if (diff_t < min_time_until_next_update_ms) {
        return last_fan_duty;
    }

    state.last_update_time_ms = now;

    // "Silent" profile
    int max_fan_duty = 40;
    int min_temp_for_duty_increase = 60;
    int max_temp = 105;
    int max_fan_duty_step = 1;
    int calculated_fan_duty = MAX(MIN_FAN_DUTY,
                                  get_point_along_line(min_temp_for_duty_increase, MIN_FAN_DUTY,
                                                       max_temp,
                                                       max_fan_duty, temp));
    int fan_duty_delta = calculated_fan_duty - last_fan_duty;
    int next_fan_duty = last_fan_duty + CLAMP(fan_duty_delta, -1 * max_fan_duty_step,
                                              max_fan_duty_step);
//    printf("Temp=%i calculated_fan_duty=%i%% next_fan_duty=%i%%\n", temp,
//           calculated_fan_duty, next_fan_duty);

    // Determine the fan speed change delta.
    int fan_speed_change_delta = next_fan_duty - last_fan_duty;

    // If the speed changed in a different direction then updated the relevant variable.
    if ((fan_speed_change_delta ^ state.last_speed_change_delta) < 0){
        state.last_speed_change_delta = next_fan_duty - last_fan_duty;
        state.last_speed_change_direction_time_ms = now;
    }

    // Determine time difference between last speed direction change.
    int64_t diff_speed_direction_change_t = now - state.last_speed_change_direction_time_ms;
    int min_time_until_next_direction_change_ms = 5000;
    if (diff_speed_direction_change_t < min_time_until_next_direction_change_ms) {
        return last_fan_duty;
    }

    return next_fan_duty;
}

static int ec_query_cpu_temp(void) {
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void) {
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_fan_duty(void) {
    int raw_duty = ec_io_read(EC_REG_FAN_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_fan_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FAN_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FAN_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_write_fan_duty(int duty_percentage) {
    if (duty_percentage < 0 || duty_percentage > 100) {
        printf("Wrong fan duty to write: %d\n", duty_percentage);
        return EXIT_FAILURE;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0;
    int v_i = (int) v_d;
    return ec_io_do(0x99, 0x01, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
                      const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
    }
    if (i >= 100) {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
               port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static uint8_t ec_io_read(const uint32_t port) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    //wait_ec(EC_SC, EC_SC_IBF_FREE);
    ec_io_wait(EC_SC, OBF, 1);
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
                    const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty) {
    return (int) ((double) raw_duty / 255.0 * 100.0);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low) {
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char *proc_name) {
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR *dir;
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char *endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE *fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0')
                    && strncmp(buf, proc_name, proc_name_len) == 0) {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char *buffer, size_t max, const char *format) {
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler) {
    signal(SIGHUP, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
    signal(SIGPIPE, handler);
    signal(SIGALRM, handler);
    signal(SIGTERM, handler);
    signal(SIGUSR1, handler);
    signal(SIGUSR2, handler);
}

static int64_t millis() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    return ((int64_t) now.tv_sec) * 1000 + ((int64_t) now.tv_nsec) / 1000000;
}

static float get_point_along_line(float x0, float y0, float x1, float y1, float xp) {
    return (y0 + ((y1 - y0) / (x1 - x0)) * (xp - x0));
}
