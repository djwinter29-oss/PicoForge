#pragma once
#include "../serial_fake.h"
typedef struct { uint index; } spi_inst_t;
extern spi_inst_t serial_spi[2];
#define spi0 (&serial_spi[0])
#define spi1 (&serial_spi[1])
#define SPI_CPOL_0 0
#define SPI_CPHA_0 0
#define SPI_MSB_FIRST 0
static inline uint spi_get_index(spi_inst_t *spi) { return spi->index; }
static inline uint spi_get_baudrate(spi_inst_t *spi) { (void)spi; return serial_fake.hz; }
typedef struct { uint32_t dr; } spi_hw_t;
static inline spi_hw_t *spi_get_hw(spi_inst_t *spi)
{ (void)spi; static spi_hw_t hardware; return &hardware; }
static inline bool spi_is_writable(spi_inst_t *spi) { (void)spi; return !serial_fake.fifo_full; }
static inline bool spi_is_readable(spi_inst_t *spi)
{
    spi_hw_t *hardware = spi_get_hw(spi);
    if (serial_fake.length < sizeof(serial_fake.bytes))
        serial_fake.bytes[serial_fake.length++] = (uint8_t)hardware->dr;
    hardware->dr = serial_fake.input;
    return !serial_fake.short_io;
}
static inline uint spi_init(spi_inst_t *spi, uint hz)
{ (void)spi; serial_fake.init++; return hz; }
static inline void spi_deinit(spi_inst_t *spi) { (void)spi; serial_fake.deinit++; }
static inline uint spi_set_baudrate(spi_inst_t *spi, uint hz)
{ (void)spi; serial_fake.baud++; serial_fake.hz = hz; return hz; }
static inline void spi_set_format(spi_inst_t *spi, uint bits, uint polarity, uint phase, uint order)
{ (void)spi; (void)bits; (void)polarity; (void)phase; (void)order; }
static inline int spi_write_read_blocking(spi_inst_t *spi, const uint8_t *tx, uint8_t *rx, size_t size)
{
    (void)spi;
    for (size_t i = 0; i < size; i++) {
        if (serial_fake.length < sizeof(serial_fake.bytes)) serial_fake.bytes[serial_fake.length++] = tx[i];
        rx[i] = serial_fake.input;
    }
    return serial_fake.short_io ? 0 : (int)size;
}
static inline int spi_write_blocking(spi_inst_t *spi, const uint8_t *tx, size_t size)
{
    (void)spi;
    for (size_t i = 0; i < size; i++)
        if (serial_fake.length < sizeof(serial_fake.bytes)) serial_fake.bytes[serial_fake.length++] = tx[i];
    return serial_fake.short_io ? 0 : (int)size;
}
static inline int spi_read_blocking(spi_inst_t *spi, uint8_t tx, uint8_t *rx, size_t size)
{
    (void)spi;
    for (size_t i = 0; i < size; i++) {
        if (serial_fake.length < sizeof(serial_fake.bytes)) serial_fake.bytes[serial_fake.length++] = tx;
        rx[i] = serial_fake.input;
    }
    return serial_fake.short_io ? 0 : (int)size;
}
