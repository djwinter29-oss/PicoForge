#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct { unsigned index; } spi_inst_t;
extern spi_inst_t test_spi;
#define spi0 (&test_spi)
#define spi1 (&test_spi)
#define SPI_CPOL_0 0
#define SPI_CPHA_0 0
#define SPI_MSB_FIRST 0
static inline unsigned spi_get_index(spi_inst_t *spi) { return spi->index; }
typedef struct { uint32_t dr; } spi_hw_t;
static inline spi_hw_t *spi_get_hw(spi_inst_t *spi)
{ (void)spi; static spi_hw_t hardware; return &hardware; }
static inline bool spi_is_writable(spi_inst_t *spi) { (void)spi; return true; }
static inline bool spi_is_readable(spi_inst_t *spi) { (void)spi; return true; }
static inline unsigned spi_init(spi_inst_t *spi, unsigned hz) { (void)spi; return hz; }
static inline void spi_deinit(spi_inst_t *spi) { (void)spi; }
static inline unsigned spi_set_baudrate(spi_inst_t *spi, unsigned hz) { (void)spi; return hz; }
static inline void spi_set_format(spi_inst_t *spi, int bits, int polarity, int phase, int order)
{ (void)spi; (void)bits; (void)polarity; (void)phase; (void)order; }
int spi_write_blocking(spi_inst_t *, const uint8_t *, size_t);
int spi_read_blocking(spi_inst_t *, uint8_t, uint8_t *, size_t);
int spi_write_read_blocking(spi_inst_t *, const uint8_t *, uint8_t *, size_t);
