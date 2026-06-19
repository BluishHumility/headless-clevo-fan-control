#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <unistd.h>

#define NAME "clevo-fan-control"

/* EC ports */
#define EC_SC   0x66
#define EC_DATA 0x62

#define EC_SC_READ_CMD 0x80

/* EC registers */
#define EC_REG_CPU_TEMP     0x07
#define EC_REG_GPU_TEMP     0xCD
#define EC_REG_FAN_DUTY     0xCE
#define EC_REG_FAN_RPM_HI   0xD0
#define EC_REG_FAN_RPM_LO   0xD1

#define MAX(a,b) ((a) > (b) ? (a) : (b))

typedef struct {
    int cpu_temp;
    int gpu_temp;
    int fan_duty;
    int fan_rpms;
} state_t;

static state_t state;

/* ---------- EC ---------- */

static int ec_wait(uint32_t port, uint32_t flag, int value)
{
    uint8_t data = inb(port);
    int i = 0;

    while ((((data >> flag) & 1) != value) && i++ < 100) {
        usleep(1000);
        data = inb(port);
    }

    return (i >= 100) ? -1 : 0;
}

static uint8_t ec_read(uint8_t reg)
{
    ec_wait(EC_SC, 1, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_wait(EC_SC, 1, 0);
    outb(reg, EC_DATA);

    ec_wait(EC_SC, 0, 1);

    return inb(EC_DATA);
}

static int ec_write_cmd(uint8_t cmd, uint8_t port, uint8_t value)
{
    ec_wait(EC_SC, 1, 0);
    outb(cmd, EC_SC);

    ec_wait(EC_SC, 1, 0);
    outb(port, EC_DATA);

    ec_wait(EC_SC, 1, 0);
    outb(value, EC_DATA);

    return ec_wait(EC_SC, 1, 0);
}

static int ec_set_fan(int duty)
{
    if (duty < 0 || duty > 100)
        return -1;

    int val = duty * 255 / 100;

    return ec_write_cmd(0x99, 0x01, (uint8_t)val);
}

static int calc_duty(int raw)
{
    return raw * 100 / 255;
}

static int calc_rpm(int hi, int lo)
{
    int v = (hi << 8) | lo;

    return v ? (2156220 / v) : 0;
}

static int ec_init(void)
{
    if (ioperm(EC_SC, 1, 1) || ioperm(EC_DATA, 1, 1)) {
        perror("ioperm");
        return -1;
    }

    return 0;
}

/* ---------- sensors ---------- */

static void ec_read_state(void)
{
    state.cpu_temp = ec_read(EC_REG_CPU_TEMP);
    state.gpu_temp = ec_read(EC_REG_GPU_TEMP);

    state.fan_duty = calc_duty(ec_read(EC_REG_FAN_DUTY));

    int hi = ec_read(EC_REG_FAN_RPM_HI);
    int lo = ec_read(EC_REG_FAN_RPM_LO);

    state.fan_rpms = calc_rpm(hi, lo);
}

/* ---------- fan curve ---------- */

static int auto_fan(void)
{
    static int duty = 10;

    int temp = MAX(state.cpu_temp, state.gpu_temp);

    switch (duty) {

    case 10:
        if (temp >= 55)
            duty = 15;
        break;

    case 15:
        if (temp >= 65)
            duty = 25;
        else if (temp <= 45)
            duty = 10;
        break;

    case 25:
        if (temp >= 75)
            duty = 40;
        else if (temp <= 55)
            duty = 15;
        break;

    case 40:
        if (temp >= 85)
            duty = 60;
        else if (temp <= 65)
            duty = 25;
        break;

    case 60:
        if (temp >= 95)
            duty = 80;
        else if (temp <= 75)
            duty = 40;
        break;

    case 80:
        if (temp >= 100)
            duty = 100;
        else if (temp <= 85)
            duty = 60;
        break;

    case 100:
        if (temp <= 95)
            duty = 80;
        break;
    }

    return duty;
}

/* ---------- info ---------- */

static void print_info(void)
{
    ec_read_state();

    printf("CPU: %d°C\n", state.cpu_temp);
    printf("GPU: %d°C\n", state.gpu_temp);
    printf("Fan duty: %d%%\n", state.fan_duty);
    printf("Fan RPM: %d\n", state.fan_rpms);
}

/* ---------- daemon ---------- */

static void daemon_loop(void)
{
    int last_duty = -1;

    printf("Starting fan daemon\n");
    fflush(stdout);

    while (1) {

        ec_read_state();

        int duty = auto_fan();

        if (duty != last_duty) {

            ec_set_fan(duty);

            printf("CPU=%d°C GPU=%d°C FAN=%d%% RPM=%d\n",
                   state.cpu_temp,
                   state.gpu_temp,
                   duty,
                   state.fan_rpms);

            fflush(stdout);

            last_duty = duty;
        }

        sleep(1);
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (ec_init() != 0)
        return EXIT_FAILURE;

    if (argc > 1 && strcmp(argv[1], "--info") == 0) {
        print_info();
        return EXIT_SUCCESS;
    }

    if (argc > 1) {

        int duty = atoi(argv[1]);

        if (duty < 0 || duty > 100) {
            fprintf(stderr, "Invalid duty\n");
            return EXIT_FAILURE;
        }

        ec_set_fan(duty);
        print_info();

        return EXIT_SUCCESS;
    }

    daemon_loop();

    return EXIT_SUCCESS;
}
