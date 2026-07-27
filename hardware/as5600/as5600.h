/*
 * as5600.h — AS5600 Magnetic Encoder Driver for Linux (RK3588)
 *
 * Provides register definitions, constants, and public API for the AS5600
 * 12-bit magnetic rotary position sensor over I2C.
 *
 * I2C Address: 0x36
 * Resolution: 12-bit (0-4095), 0.0879 degrees per LSB
 */

#ifndef __AS5600_H
#define __AS5600_H

#include <stdint.h>

/* ================================================================
 * I2C Device Address
 * ================================================================ */
#define AS5600_I2C_ADDR          0x36

/* ================================================================
 * Register Map (15 registers)
 * ================================================================ */
typedef enum {
    AS5600_REG_ZMCO        = 0x00,  /* Burn count (number of times burned) */
    AS5600_REG_ZPOS_HI     = 0x01,  /* Start position, high byte */
    AS5600_REG_ZPOS_LO     = 0x02,  /* Start position, low byte */
    AS5600_REG_MPOS_HI     = 0x03,  /* End position, high byte */
    AS5600_REG_MPOS_LO     = 0x04,  /* End position, low byte */
    AS5600_REG_MANG_HI     = 0x05,  /* Maximum angle, high byte */
    AS5600_REG_MANG_LO     = 0x06,  /* Maximum angle, low byte */
    AS5600_REG_CONF_HI     = 0x07,  /* Configuration, high byte */
    AS5600_REG_CONF_LO     = 0x08,  /* Configuration, low byte */
    /* 0x09-0x0A reserved */
    AS5600_REG_STATUS      = 0x0B,  /* Status register */
    AS5600_REG_RAW_ANGLE_HI = 0x0C, /* Raw angle, high byte (bits 11:8) */
    AS5600_REG_RAW_ANGLE_LO = 0x0D, /* Raw angle, low byte (bits 7:0)  */
    AS5600_REG_ANGLE_HI    = 0x0E,  /* Scaled angle, high byte */
    AS5600_REG_ANGLE_LO    = 0x0F,  /* Scaled angle, low byte */
    /* 0x10-0x19 reserved */
    AS5600_REG_AGC         = 0x1A,  /* Automatic Gain Control */
    AS5600_REG_MAGNITUDE_HI = 0x1B, /* Magnitude, high byte */
    AS5600_REG_MAGNITUDE_LO = 0x1C, /* Magnitude, low byte */
    /* 0x1D-0xFE reserved */
    AS5600_REG_BURN        = 0xFF   /* Burn command register */
} as5600_reg_t;

/* ================================================================
 * Status Register Bit Masks (AS5600_REG_STATUS)
 * ================================================================
 * Bits: 0 0 MD ML MH 0 0 0
 */
#define AS5600_STATUS_MH           0x20  /* Magnet detected (too strong → MD, too weak → ML) */
#define AS5600_STATUS_ML           0x10  /* AGC minimum overflow — magnet too weak */
#define AS5600_STATUS_MD           0x08  /* AGC maximum overflow — magnet too strong */

/* ================================================================
 * Magnet Strength Levels
 * ================================================================ */
#define AS5600_MAGNET_NONE         0  /* No magnet detected */
#define AS5600_MAGNET_TOO_WEAK     1  /* Magnet too weak (ML set) */
#define AS5600_MAGNET_JUST_RIGHT   2  /* Magnet strength OK */
#define AS5600_MAGNET_TOO_STRONG   3  /* Magnet too strong (MD set) */

/* ================================================================
 * Burn Commands (written to AS5600_REG_BURN)
 * ================================================================ */
#define AS5600_BURN_ANGLE          0x80  /* Burn ZPOS and MPOS (max 3 times) */
#define AS5600_BURN_SETTINGS       0x40  /* Burn MANG and CONFIG (max 1 time) */

/* ================================================================
 * Conversions
 * ================================================================ */
#define AS5600_RAW_TO_DEGREES(raw) ((float)(raw) * 0.087890625f)  /* 360.0 / 4096.0 */

/* ================================================================
 * Opaque Device Handle
 * ================================================================ */
typedef struct as5600_dev *as5600_dev_t;

/* ================================================================
 * Public API
 * ================================================================ */

/* ---- Lifecycle ---- */
as5600_dev_t  as5600_open(const char *i2c_path, uint8_t addr);
void          as5600_close(as5600_dev_t dev);
int           as5600_probe(as5600_dev_t dev);

/* ---- Angle Reading ---- */
uint16_t      as5600_get_raw_angle(as5600_dev_t dev);
uint16_t      as5600_get_scaled_angle(as5600_dev_t dev);
float         as5600_convert_raw_angle_to_degrees(uint16_t raw);

/* ---- Position Configuration ---- */
uint16_t      as5600_get_start_position(as5600_dev_t dev);
uint16_t      as5600_get_end_position(as5600_dev_t dev);
uint16_t      as5600_get_max_angle(as5600_dev_t dev);
uint16_t      as5600_set_start_position(as5600_dev_t dev, int16_t angle);
uint16_t      as5600_set_end_position(as5600_dev_t dev, int16_t angle);
uint16_t      as5600_set_max_angle(as5600_dev_t dev, int16_t angle);

/* ---- Status & Diagnostics ---- */
uint8_t       as5600_get_agc(as5600_dev_t dev);
uint16_t      as5600_get_magnitude(as5600_dev_t dev);
uint8_t       as5600_get_status(as5600_dev_t dev);
uint8_t       as5600_detect_magnet(as5600_dev_t dev);
uint8_t       as5600_get_magnet_strength(as5600_dev_t dev);
uint8_t       as5600_get_burn_count(as5600_dev_t dev);

/* ---- Burn (Permanent) ---- */
int           as5600_burn_angle(as5600_dev_t dev);
int           as5600_burn_max_angle_and_config(as5600_dev_t dev);

/* ---- Debug ---- */
void          as5600_dump_all(as5600_dev_t dev);

#endif /* __AS5600_H */
