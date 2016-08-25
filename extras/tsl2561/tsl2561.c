/*
 * Part of esp-open-rtos
 * Copyright (C) 2016 Brian Schwind (https://github.com/bschwind)
 * BSD Licensed as described in the file LICENSE
 */

#include <stdio.h>
#include "FreeRTOS.h"
#include "i2c/i2c.h"
#include "task.h"
#include "tsl2561.h"

bool write_register(uint8_t i2c_addr, uint8_t reg, uint8_t value)
{
    uint8_t data[2];
    data[0] = TSL2561_REG_COMMAND | reg;
    data[1] = value;
    return i2c_slave_write(i2c_addr, data, 2);
}

uint8_t read_register(uint8_t i2c_addr, uint8_t reg)
{
    uint8_t data[1];

    if (!i2c_slave_read(i2c_addr, TSL2561_REG_COMMAND | reg, data, 1))
    {
        printf("Error in tsl261 read_register\n");
    }

    return data[0];
}

uint16_t read_register_16(uint8_t i2c_addr, uint8_t low_register_addr)
{
    uint16_t value = 0;
    uint8_t data[2];

    if (!i2c_slave_read(i2c_addr, TSL2561_REG_COMMAND | TSL2561_READ_WORD | low_register_addr, data, 2))
    {
        printf("Error with i2c_slave_read in read_register_16\n");
    }

    value = ((uint16_t)data[1] << 8) | (data[0]);

    return value;
}

bool enable(uint8_t i2c_addr)
{
    return write_register(i2c_addr, TSL2561_REG_CONTROL, TSL2561_ON);
}

bool disable(uint8_t i2c_addr)
{
    return write_register(i2c_addr, TSL2561_REG_CONTROL, TSL2561_OFF);
}

void tsl2561_init(tsl2561_t *device)
{
    if (!enable(device->i2c_addr))
    {
        printf("Error initializing tsl2561\n");
    }

    uint8_t control_reg = (read_register(device->i2c_addr, TSL2561_REG_CONTROL) & TSL2561_ON);

    if (control_reg != TSL2561_ON)
    {
        printf("Error initializing tsl2561, control register wasn't set to ON\n");
    }

    // Fetch the package type
    uint8_t part_reg = read_register(device->i2c_addr, TSL2561_REG_PART_ID);
    uint8_t package = part_reg >> 6;
    device->package_type = package;

    // Fetch the gain and integration time
    uint8_t timing_register = read_register(device->i2c_addr, TSL2561_REG_TIMING);
    device->gain = timing_register & 0x10;
    device->integration_time = timing_register & 0x03;

    disable(device->i2c_addr);
}

void tsl2561_set_integration_time(tsl2561_t *device, uint8_t integration_time_id)
{
    enable(device->i2c_addr);
    write_register(device->i2c_addr, TSL2561_REG_TIMING, integration_time_id | device->gain);
    disable(device->i2c_addr);

    device->integration_time = integration_time_id;
}

void tsl2561_set_gain(tsl2561_t *device, uint8_t gain)
{
    enable(device->i2c_addr);
    write_register(device->i2c_addr, TSL2561_REG_TIMING, gain | device->integration_time);
    disable(device->i2c_addr);

    device->gain = gain;
}

void get_channel_data(tsl2561_t *device, uint16_t *channel0, uint16_t *channel1)
{
    enable(device->i2c_addr);

    // Since we just enabled the chip, we need to sleep
    // for the chip's integration time so it can gather a reading
    switch (device->integration_time)
    {
        case TSL2561_INTEGRATION_13MS:
            vTaskDelay(TSL2561_INTEGRATION_TIME_13MS / portTICK_RATE_MS);
            break;
        case TSL2561_INTEGRATION_101MS:
            vTaskDelay(TSL2561_INTEGRATION_TIME_101MS / portTICK_RATE_MS);
            break;
        default:
            vTaskDelay(TSL2561_INTEGRATION_TIME_402MS / portTICK_RATE_MS);
            break;
    }

    *channel0 = read_register_16(device->i2c_addr, TSL2561_REG_CHANNEL_0_LOW);
    *channel1 = read_register_16(device->i2c_addr, TSL2561_REG_CHANNEL_1_LOW);

    disable(device->i2c_addr);
}

bool tsl2561_read_lux(tsl2561_t *device, uint32_t *lux)
{
    uint32_t chScale;
    uint32_t channel1;
    uint32_t channel0;

    switch (device->integration_time)
    {
        case TSL2561_INTEGRATION_13MS:
            chScale = CHSCALE_TINT0;
            break;
        case TSL2561_INTEGRATION_101MS:
            chScale = CHSCALE_TINT1;
            break;
        default:
            chScale = (1 << CH_SCALE);
            break;
    }

    // Scale if gain is 1x
    if (device->gain == TSL2561_GAIN_1X)
    {
        // 16x is nominal, so if the gain is set to 1x then
        // we need to scale by 16
        chScale = chScale << 4;
    }

    uint16_t ch0;
    uint16_t ch1;
    get_channel_data(device, &ch0, &ch1);

    // Scale the channel values
    channel0 = (ch0 * chScale) >> CH_SCALE;
    channel1 = (ch1 * chScale) >> CH_SCALE;

    // Find the ratio of the channel values (channel1 / channel0)
    // Protect against divide by zero
    uint32_t ratio1 = 0;
    if (channel0 != 0)
    {
        ratio1 = (channel1 << (RATIO_SCALE+1)) / channel0;
    }

    // Round the ratio value
    uint32_t ratio = (ratio1 + 1) >> 1;

    uint32_t b;
    uint32_t m;
    switch (device->package_type)
    {
        case TSL2561_PACKAGE_CS:
            if ((ratio >= 0) && (ratio <= K1C))
            {
                b = B1C;
                m = M1C;
            }
            else if (ratio <= K2C)
            {
                b = B2C;
                m = M2C;
            }
            else if (ratio <= K3C)
            {
                b = B3C;
                m = M3C;
            }
            else if (ratio <= K4C)
            {
                b = B4C;
                m = M4C;
            }
            else if (ratio <= K5C)
            {
                b = B5C;
                m = M5C;
            }
            else if (ratio <= K6C)
            {
                b = B6C;
                m = M6C;
            }
            else if (ratio <= K7C)
            {
                b = B7C;
                m = M7C;
            }
            else if (ratio > K8C)
            {
                b = B8C;
                m = M8C;
            }

            break;
        case TSL2561_PACKAGE_T_FN_CL:
            if ((ratio >= 0) && (ratio <= K1T))
            {
                b = B1T;
                m = M1T;
            }
            else if (ratio <= K2T)
            {
                b = B2T;
                m = M2T;
            }
            else if (ratio <= K3T)
            {
                b = B3T;
                m = M3T;
            }
            else if (ratio <= K4T)
            {
                b = B4T;
                m = M4T;
            }
            else if (ratio <= K5T)
            {
                b = B5T;
                m = M5T;
            }
            else if (ratio <= K6T)
            {
                b = B6T;
                m = M6T;
            }
            else if (ratio <= K7T)
            {
                b = B7T;
                m = M7T;
            }
            else if (ratio > K8T)
            {
                b = B8T;
                m = M8T;
            }

            break;
        default:
            printf("Invalid package type in CalculateLux\n");
            b = 0;
            m = 0;
            break;
    }

    uint32_t temp;
    temp = ((channel0 * b) - (channel1 * m));

    // Do not allow negative lux value
    if (temp < 0)
    {
        temp = 0;
    }

    // Round lsb (2^(LUX_SCALE−1))
    temp += (1 << (LUX_SCALE - 1));

    // Strip off fractional portion
    *lux = temp >> LUX_SCALE;

    return true;
}
