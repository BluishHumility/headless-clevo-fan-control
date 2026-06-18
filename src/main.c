#define _GNU_SOURCE

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
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#define NAME "clevo-fan-control"

/* EC ports */
#define EC_SC   0x66
#define EC_DATA 0x62

#define EC_SC_READ_CMD 0x80

/* EC registers */
#define EC_REG_SIZE         0x100
#define EC_REG_CPU_TEMP     0x07
#define EC_REG_GPU_TEMP     0xCD
#define EC_REG_FAN_DUTY     0xCE
#define EC_REG_FAN_RPM_HI   0xD0
#define EC_REG_FAN_RPM_LO   0xD1

#define MIN_FAN_DUTY 16

#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/* ---------------- state ---------------- */

typedef struct {
    int cpu_temp;
    int gpu_temp;
    int fan_duty;
    int fan_rpms;

    int auto_mode;
    int auto_duty_val;

    int64_t last_update_ms;
} state_t;

static state_t state;

/* ---------------- timing ---------------- */

static int64_t millis(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---------------- EC low-level ---------------- */

static int ec_wait(uint32_t port, uint32_t flag, int value) {
    uint8_t data = inb(port);
    int i = 0;

    while ((((data >> flag) & 1) != value) && i++ < 100) {
        usleep(1000);
        data = inb(port);
    }

    return (i >= 100) ? -1 : 0;
}

static uint8_t ec_read(uint8_t reg) {
    ec_wait(EC_SC, 1, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_wait(EC_SC, 1, 0);
    outb(reg, EC_DATA);

    ec_wait(EC_SC, 0, 1);
    return inb(EC_DATA);
}

static int ec_write_cmd(uint8_t cmd, uint8_t port, uint8_t value) {
    ec_wait(EC_SC, 1, 0);
    outb(cmd, EC_SC);

    ec_wait(EC_SC, 1, 0);
    outb(port, EC_DATA);

    ec_wait(EC_SC, 1, 0);
    outb(value, EC_DATA);

    return ec_wait(EC_SC, 1, 0);
}

/* ---------------- fan control ---------------- */

static int ec_set_fan(int duty) {
    if (duty < 0 || duty > 100) return -1;

    int val = (int)((duty / 100.0) * 255.0);
    return ec_write_cmd(0x99, 0x01, (uint8_t)val);
}

static int calc_duty(int raw) {
    return (int)((raw / 255.0) * 100.0);
}

static int calc_rpm(int hi, int lo) {
    int v = (hi << 8) | lo;
    return v ? (2156220 / v) : 0;
}

/* ---------------- EC init ---------------- */

static int ec_init(void) {
    if (ioperm(EC_SC, 1, 1) || ioperm(EC_DATA, 1, 1)) {
        perror("ioperm");
        return -1;
    }
    return 0;
}

/* ---------------- sensor update ---------------- */

static void ec_read_state(void) {
    state.cpu_temp = ec_read(EC_REG_CPU_TEMP);
    state.gpu_temp = ec_read(EC_REG_GPU_TEMP);

    state.fan_duty = calc_duty(ec_read(EC_REG_FAN_DUTY));

    int hi = ec_read(EC_REG_FAN_RPM_HI);
    int lo = ec_read(EC_REG_FAN_RPM_LO);
    state.fan_rpms = calc_rpm(hi, lo);
}

/* ---------------- auto fan logic ---------------- */

static int auto_fan(void) {
    int temp = MAX(state.cpu_temp, state.gpu_temp);

    int last = MAX(MIN_FAN_DUTY, state.auto_duty_val);

    int now = millis();
    if (now - state.last_update_ms < 300)
        return last;

    state.last_update_ms = now;

    int max_duty = 40;

    int target =
        (temp < 60)
            ? MIN_FAN_DUTY
            : CLAMP((temp - 60), MIN_FAN_DUTY, max_duty);

    int next = last + CLAMP(target - last, -1, 1);

    return next;
}

/* ---------------- modes ---------------- */

static void print_info(void) {
    ec_read_state();

    printf("CPU: %d°C\n", state.cpu_temp);
    printf("GPU: %d°C\n", state.gpu_temp);
    printf("Fan duty: %d%%\n", state.fan_duty);
    printf("Fan RPM: %d\n", state.fan_rpms);
}

/* ---------------- main loop ---------------- */

static void daemon_loop(void) {
    state.auto_mode = 1;
    state.last_update_ms = millis();

    printf("Starting headless fan daemon...\n");

    while (1) {
        ec_read_state();

        int next = auto_fan();

        if (next != state.auto_duty_val) {
            ec_set_fan(next);
            state.auto_duty_val = next;
        }

        usleep(200 * 1000);
    }
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    if (ec_init() != 0)
        return EXIT_FAILURE;

    /* stop all instances */
    if (argc > 1 && strcmp(argv[1], "exit") == 0) {
        system("pkill -f clevo-fan-control");
        return EXIT_SUCCESS;
    }

    /* info mode */
    if (argc > 1 && strcmp(argv[1], "--info") == 0) {
        print_info();
        return EXIT_SUCCESS;
    }

    /* manual mode */
    if (argc > 1) {
        int duty = atoi(argv[1]);
        if (duty < 0 || duty > 100) {
            fprintf(stderr, "Invalid duty (0-100)\n");
            return EXIT_FAILURE;
        }

        ec_set_fan(duty);
        print_info();
        return EXIT_SUCCESS;
    }

    /* daemon mode */
    daemon_loop();

    return EXIT_SUCCESS;
}
