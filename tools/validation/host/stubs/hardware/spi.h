#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int index; } spi_inst_t;
extern spi_inst_t test_spi;
#define spi0 (&test_spi)
#define spi1 (&test_spi)
#define SPI_CPOL_0 0
#define SPI_CPHA_0 0
#define SPI_MSB_FIRST 0
static inline unsigned spi_get_index(spi_inst_t *s) { return s->index; }
static inline unsigned spi_init(spi_inst_t *s, unsigned hz) { (void)s; return hz; }
static inline unsigned spi_set_baudrate(spi_inst_t *s, unsigned hz) { (void)s; return hz; }
static inline void spi_set_format(spi_inst_t *s, int a, int b, int c, int d) { (void)s;(void)a;(void)b;(void)c;(void)d; }
int spi_write_blocking(spi_inst_t *, const uint8_t *, size_t);
int spi_read_blocking(spi_inst_t *, uint8_t, uint8_t *, size_t);
int spi_write_read_blocking(spi_inst_t *, const uint8_t *, uint8_t *, size_t);
