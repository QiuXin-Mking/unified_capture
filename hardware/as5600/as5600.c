/*
 * as5600.c — AS5600 Magnetic Encoder Driver Implementation
 *
 * Uses Linux I2C ioctl(I2C_RDWR) for zero-library dependency.
 * Repeated START is handled natively by sending two i2c_msg structs
 * in a single ioctl call.
 *
 * All I2C primitives are static (file-local) and use the i2c_ prefix.
 * Only the business-level as5600_* functions are part of the public API.
 *
 * CRITICAL PROTOCOL NOTE:
 *   Reading a 12-bit register pair requires reading LO first, then HI.
 *   The AS5600 latches the HI byte when LO is read.
 */

#include "as5600.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

/* ---- Linux I2C headers ---- */
#include <linux/i2c-dev.h>
#include <linux/i2c.h>

/* ================================================================
 * Internal Device Structure (opaque to user)
 * ================================================================ */
struct as5600_dev {
    int      fd;    /* File descriptor for /dev/i2c-N */
    uint8_t  addr;  /* I2C slave address (0x36) */
};

/* ================================================================
 * Internal I2C Primitives (static, file-local only)
 *
 * These four functions are the ONLY code that touches Linux I2C.
 * All public API functions go through them — never ioctl directly.
 * ================================================================ */

static char i2c_err_buf[256];

static const char *i2c_strerror(void)
{
    snprintf(i2c_err_buf, sizeof(i2c_err_buf), "I2C error: %s (%d)", strerror(errno), errno);
    return i2c_err_buf;
}

/*
 * i2c_read_byte — Read a single byte from a register.
 *
 * Protocol: Write reg addr → Repeated START → Read 1 byte
 * Returns:  0 on success, -1 on error
 */
static int i2c_read_byte(as5600_dev_t dev, uint8_t reg, uint8_t *val)
{
    struct i2c_msg msgs[2] = {
        {
            .addr  = dev->addr,
            .flags = 0,              /* write */
            .len   = 1,
            .buf   = &reg,
        },
        {
            .addr  = dev->addr,
            .flags = I2C_M_RD,       /* read */
            .len   = 1,
            .buf   = val,
        },
    };

    struct i2c_rdwr_ioctl_data ioctl_data = {
        .msgs  = msgs,
        .nmsgs = 2,
    };

    if (ioctl(dev->fd, I2C_RDWR, &ioctl_data) < 0) {
        fprintf(stderr, "i2c_read_byte(reg=0x%02X): %s\n", reg, i2c_strerror());
        return -1;
    }
    return 0;
}

/*
 * i2c_write_byte — Write a single byte to a register.
 *
 * Protocol: Write reg addr + data in a single msg
 * Returns:  0 on success, -1 on error
 */
static int i2c_write_byte(as5600_dev_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };

    struct i2c_msg msgs[1] = {
        {
            .addr  = dev->addr,
            .flags = 0,              /* write */
            .len   = 2,
            .buf   = buf,
        },
    };

    struct i2c_rdwr_ioctl_data ioctl_data = {
        .msgs  = msgs,
        .nmsgs = 1,
    };

    if (ioctl(dev->fd, I2C_RDWR, &ioctl_data) < 0) {
        fprintf(stderr, "i2c_write_byte(reg=0x%02X, val=0x%02X): %s\n",
                reg, val, i2c_strerror());
        return -1;
    }
    return 0;
}

/* ---- Word helpers (file-local) ---- */
static inline uint8_t high_byte(uint16_t v) { return (uint8_t)(v >> 8); }
static inline uint8_t low_byte(uint16_t v)  { return (uint8_t)(v & 0xFF); }

static inline uint16_t make_word(uint8_t hi, uint8_t lo)
{
    return ((uint16_t)hi << 8) | lo;
}

/*
 * i2c_read_word — Read a 12-bit value from a register pair.
 *
 * CRITICAL: LO byte must be read FIRST to latch the HI byte
 * inside the AS5600.  This matches the STM32 reference code.
 */
static uint16_t i2c_read_word(as5600_dev_t dev, uint8_t reg_hi, uint8_t reg_lo)
{
    uint8_t lo, hi;

    /* Read LO first — this latches HI internally */
    if (i2c_read_byte(dev, reg_lo, &lo) < 0)
        return 0;

    /* Then read HI */
    if (i2c_read_byte(dev, reg_hi, &hi) < 0)
        return 0;

    return make_word(hi, lo);
}

/*
 * i2c_write_word — Write a 12-bit value to a register pair.
 *
 * HI byte is written first, then a 2ms EEPROM write delay,
 * then LO byte.
 */
static int i2c_write_word(as5600_dev_t dev, uint8_t reg_hi, uint8_t reg_lo, uint16_t val)
{
    if (i2c_write_byte(dev, reg_hi, high_byte(val)) < 0)
        return -1;

    usleep(2000);  /* EEPROM write cycle delay */

    if (i2c_write_byte(dev, reg_lo, low_byte(val)) < 0)
        return -1;

    usleep(2000);

    return 0;
}

/* ================================================================
 * Public API — Lifecycle
 * ================================================================ */

as5600_dev_t as5600_open(const char *i2c_path, uint8_t addr)
{
    struct as5600_dev *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        fprintf(stderr, "as5600_open: malloc failed\n");
        return NULL;
    }

    dev->fd = open(i2c_path, O_RDWR);
    if (dev->fd < 0) {
        fprintf(stderr, "as5600_open: cannot open %s: %s\n", i2c_path, i2c_strerror());
        free(dev);
        return NULL;
    }

    dev->addr = addr;
    return dev;
}

void as5600_close(as5600_dev_t dev)
{
    if (dev) {
        if (dev->fd >= 0)
            close(dev->fd);
        free(dev);
    }
}

/*
 * as5600_probe — Verify the AS5600 is present on the bus.
 *
 * Reads the AGC register (0x1A).  If the chip is absent, the
 * ioctl will return an error.  No actual value check is needed
 * because different magnet configurations yield different AGC
 * values — what matters is that the I2C transaction succeeds.
 *
 * Returns: 0 if chip is detected, -1 if not
 */
int as5600_probe(as5600_dev_t dev)
{
    uint8_t agc;
    if (i2c_read_byte(dev, AS5600_REG_AGC, &agc) < 0)
        return -1;
    return 0;
}

/* ================================================================
 * Public API — Angle Reading
 * ================================================================ */

uint16_t as5600_get_raw_angle(as5600_dev_t dev)
{
    return i2c_read_word(dev, AS5600_REG_RAW_ANGLE_HI, AS5600_REG_RAW_ANGLE_LO);
}

uint16_t as5600_get_scaled_angle(as5600_dev_t dev)
{
    return i2c_read_word(dev, AS5600_REG_ANGLE_HI, AS5600_REG_ANGLE_LO);
}

float as5600_convert_raw_angle_to_degrees(uint16_t raw)
{
    return (float)raw * 0.087890625f;  /* 360 / 4096 */
}

/* ================================================================
 * Public API — Position Configuration
 * ================================================================ */

uint16_t as5600_get_start_position(as5600_dev_t dev)
{
    return i2c_read_word(dev, AS5600_REG_ZPOS_HI, AS5600_REG_ZPOS_LO);
}

uint16_t as5600_get_end_position(as5600_dev_t dev)
{
    return i2c_read_word(dev, AS5600_REG_MPOS_HI, AS5600_REG_MPOS_LO);
}

uint16_t as5600_get_max_angle(as5600_dev_t dev)
{
    return i2c_read_word(dev, AS5600_REG_MANG_HI, AS5600_REG_MANG_LO);
}

uint16_t as5600_set_start_position(as5600_dev_t dev, int16_t angle)
{
    uint16_t val;
    if (angle == -1)
        val = as5600_get_raw_angle(dev);
    else
        val = (uint16_t)angle;

    i2c_write_word(dev, AS5600_REG_ZPOS_HI, AS5600_REG_ZPOS_LO, val);
    return as5600_get_start_position(dev);
}

uint16_t as5600_set_end_position(as5600_dev_t dev, int16_t angle)
{
    uint16_t val;
    if (angle == -1)
        val = as5600_get_raw_angle(dev);
    else
        val = (uint16_t)angle;

    i2c_write_word(dev, AS5600_REG_MPOS_HI, AS5600_REG_MPOS_LO, val);
    return as5600_get_end_position(dev);
}

uint16_t as5600_set_max_angle(as5600_dev_t dev, int16_t angle)
{
    uint16_t val;
    if (angle == -1)
        val = as5600_get_raw_angle(dev);
    else
        val = (uint16_t)angle;

    i2c_write_word(dev, AS5600_REG_MANG_HI, AS5600_REG_MANG_LO, val);
    return as5600_get_max_angle(dev);
}

/* ================================================================
 * Public API — Status & Diagnostics
 * ================================================================ */

uint8_t as5600_get_agc(as5600_dev_t dev)
{
    uint8_t val = 0;
    i2c_read_byte(dev, AS5600_REG_AGC, &val);
    return val;
}

uint16_t as5600_get_magnitude(as5600_dev_t dev)
{
    return i2c_read_word(dev, AS5600_REG_MAGNITUDE_HI, AS5600_REG_MAGNITUDE_LO);
}

uint8_t as5600_get_status(as5600_dev_t dev)
{
    uint8_t val = 0;
    i2c_read_byte(dev, AS5600_REG_STATUS, &val);
    return val;
}

uint8_t as5600_detect_magnet(as5600_dev_t dev)
{
    uint8_t status = as5600_get_status(dev);
    return (status & AS5600_STATUS_MH) ? 1 : 0;
}

/*
 * as5600_get_magnet_strength — Assess magnet field strength.
 *
 * Status bits: MD (0x08) = too strong, ML (0x10) = too weak, MH (0x20) = detected
 *
 * Returns:
 *   0 — no magnet
 *   1 — too weak (ML set)
 *   2 — just right (MH set, ML and MD clear)
 *   3 — too strong (MD set)
 */
uint8_t as5600_get_magnet_strength(as5600_dev_t dev)
{
    uint8_t status = as5600_get_status(dev);

    if (!(status & AS5600_STATUS_MH))
        return AS5600_MAGNET_NONE;       /* No magnet */

    if (status & AS5600_STATUS_ML)
        return AS5600_MAGNET_TOO_WEAK;   /* Too weak */

    if (status & AS5600_STATUS_MD)
        return AS5600_MAGNET_TOO_STRONG; /* Too strong */

    return AS5600_MAGNET_JUST_RIGHT;     /* OK */
}

uint8_t as5600_get_burn_count(as5600_dev_t dev)
{
    uint8_t val = 0;
    i2c_read_byte(dev, AS5600_REG_ZMCO, &val);
    return val;
}

/* ================================================================
 * Public API — Burn (Permanent)
 * ================================================================ */

/*
 * as5600_burn_angle — Permanently burn ZPOS and MPOS to OTP.
 *
 * This can be done at most 3 times (ZMCO < 3).
 *
 * Returns:
 *    1 — success
 *   -1 — no magnet detected
 *   -2 — burn limit exceeded (ZMCO >= 3)
 *   -3 — start and end positions both zero (useless burn)
 */
int as5600_burn_angle(as5600_dev_t dev)
{
    if (!as5600_detect_magnet(dev))
        return -1;

    if (as5600_get_burn_count(dev) >= 3)
        return -2;

    uint16_t zpos = as5600_get_start_position(dev);
    uint16_t mpos = as5600_get_end_position(dev);

    if (zpos == 0 && mpos == 0)
        return -3;

    i2c_write_byte(dev, AS5600_REG_BURN, AS5600_BURN_ANGLE);
    return 1;
}

/*
 * as5600_burn_max_angle_and_config — Permanently burn MANG and CONFIG to OTP.
 *
 * This can be done at most 1 time (ZMCO must be 0).
 * MANG must be at least 18 degrees (raw value >= 205).
 *
 * Returns:
 *    1 — success
 *   -1 — burn limit exceeded (already burned settings)
 *   -2 — max angle too small (< 18 degrees)
 */
int as5600_burn_max_angle_and_config(as5600_dev_t dev)
{
    if (as5600_get_burn_count(dev) != 0)
        return -1;

    uint16_t mangle = as5600_get_max_angle(dev);

    /* Minimum 18 degrees: 18 / 0.087890625 ≈ 205 */
    if (as5600_convert_raw_angle_to_degrees(mangle) < 18.0f)
        return -2;

    i2c_write_byte(dev, AS5600_REG_BURN, AS5600_BURN_SETTINGS);
    return 1;
}

/* ================================================================
 * Public API — Debug
 * ================================================================ */

void as5600_dump_all(as5600_dev_t dev)
{
    uint8_t  b;
    uint16_t w;
    uint8_t  status;

    printf("=================================\n");
    printf(" AS5600 Register Dump\n");
    printf("=================================\n");

    /* ZMCO */
    i2c_read_byte(dev, AS5600_REG_ZMCO, &b);
    printf("  ZMCO       (0x00): %u\n", b);

    /* ZPOS (12-bit) */
    w = as5600_get_start_position(dev);
    printf("  ZPOS  (0x01-02): %u (%.2f deg)\n", w, as5600_convert_raw_angle_to_degrees(w));

    /* MPOS (12-bit) */
    w = as5600_get_end_position(dev);
    printf("  MPOS  (0x03-04): %u (%.2f deg)\n", w, as5600_convert_raw_angle_to_degrees(w));

    /* MANG (12-bit) */
    w = as5600_get_max_angle(dev);
    printf("  MANG  (0x05-06): %u (%.2f deg)\n", w, as5600_convert_raw_angle_to_degrees(w));

    /* CONF (16-bit) */
    b = 0;
    i2c_read_byte(dev, AS5600_REG_CONF_HI, &b);
    printf("  CONF_HI   (0x07): 0x%02X\n", b);
    i2c_read_byte(dev, AS5600_REG_CONF_LO, &b);
    printf("  CONF_LO   (0x08): 0x%02X\n", b);

    /* STATUS */
    status = as5600_get_status(dev);
    printf("  STATUS    (0x0B): 0x%02X (MH=%d ML=%d MD=%d)\n",
           status,
           !!(status & AS5600_STATUS_MH),
           !!(status & AS5600_STATUS_ML),
           !!(status & AS5600_STATUS_MD));

    /* RAW ANGLE (12-bit) */
    w = as5600_get_raw_angle(dev);
    printf("  RAW_ANGLE (0x0C-0D): %u (%.2f deg)\n", w, as5600_convert_raw_angle_to_degrees(w));

    /* SCALED ANGLE (12-bit) */
    w = as5600_get_scaled_angle(dev);
    printf("  ANGLE     (0x0E-0F): %u (%.2f deg)\n", w, as5600_convert_raw_angle_to_degrees(w));

    /* AGC */
    printf("  AGC       (0x1A): %u\n", as5600_get_agc(dev));

    /* MAGNITUDE (12-bit) */
    w = as5600_get_magnitude(dev);
    printf("  MAGNITUDE (0x1B-1C): %u\n", w);

    /* Magnet strength summary */
    uint8_t strength = as5600_get_magnet_strength(dev);
    const char *strength_str[] = { "None", "Too Weak", "Just Right", "Too Strong" };
    printf("\n  Magnet: %s\n", strength_str[strength & 0x03]);

    printf("=================================\n");
}
