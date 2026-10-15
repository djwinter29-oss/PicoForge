#include <assert.h>
#include <stdio.h>
#include "../../../firmware/src/drivers/sd_card/spi/sd_spi.c"
spi_inst_t test_spi;
static uint8_t queue[32];
static unsigned pos, len, writes, stops;
static bool busy, selected;
static int short_size, short_exchange;
static unsigned exchanges;
uint32_t time_us_32(void) { static uint32_t t; return t += 100000u; }
void gpio_put(unsigned pin, bool high) { (void)pin; selected = !high; }
int spi_write_blocking(spi_inst_t *s, const uint8_t *p, size_t n)
{ (void)s;(void)p; ++writes; return (int)n == short_size ? 0 : (int)n; }
int spi_read_blocking(spi_inst_t *s, uint8_t v, uint8_t *p, size_t n)
{ (void)s;(void)v; memset(p, 0, n); return (int)n; }
int spi_write_read_blocking(spi_inst_t *s, const uint8_t *p, uint8_t *r, size_t n)
{ (void)s; assert(n == 1);
  if ((int)exchanges++ == short_exchange) return 0;
  if (*p == 0xfdu) ++stops;
  *r = pos < len ? queue[pos++] : busy ? 0 : 0xff; return 1; }
static void reset(uint8_t response)
{
 card_initialized = card_block_addressed = true; card_sector_count = 128;
 active_config.spi = &test_spi; active_config.use_card_detect = false;
 pos = 0; len = 5; queue[0]=0xff; queue[1]=0; queue[2]=0xff; queue[3]=0xff; queue[4]=response;
 busy = selected = false; short_size = -1; writes = stops = exchanges = 0; short_exchange = -1;
}
int main(void)
{
 uint8_t data[1024] = {0};
 reset(5); assert(pf_sd_spi_write_blocks(0,data,1)==PF_SD_CARD_OK); assert(card_initialized && !selected);
 for (unsigned count=1; count<=2; ++count) {
  for (unsigned i=0; i<2; ++i) {
   reset(i ? 0x0d : 0x0b);
   assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
   assert(card_initialized && !selected); assert(pf_sd_spi_sync()==PF_SD_CARD_OK);
   assert(stops == (count==2));
  }
  reset(5); busy=true;
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
  assert(!card_initialized && !selected);
  unsigned before=writes;
  assert(pf_sd_spi_sync()==PF_SD_CARD_NOT_READY);
  assert(pf_sd_spi_write_blocks(0,data,1)==PF_SD_CARD_NOT_READY && writes==before);
  reset(0x09);
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
  assert(!card_initialized && !selected);
  reset(0x0b); busy=true;
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
  assert(!card_initialized && !selected);
  reset(0xff);
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
  assert(!card_initialized && !selected);
  reset(5); short_size=512;
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
  assert(!card_initialized && !selected);
 }
 /* Failure at initial ready, R1, either CRC byte, response, or busy. */
 for (unsigned count=1; count<=2; ++count) {
  for (int failure=0; failure<=5; ++failure) {
   reset(5); short_exchange=failure;
   assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR);
   assert(!card_initialized && !selected);
   assert(pf_sd_spi_sync()==PF_SD_CARD_NOT_READY);
  }
  reset(5); short_size=6;
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR && !card_initialized);
  reset(5); short_size=1;
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR && !card_initialized);
  reset(5); len=1; /* Command response never arrives. */
  assert(pf_sd_spi_write_blocks(0,data,count)==PF_SD_CARD_IO_ERROR && !card_initialized);
 }
 reset(5); len=9;
 queue[5]=0xff; queue[6]=0xff; queue[7]=0xff; queue[8]=5;
 assert(pf_sd_spi_write_blocks(0,data,2)==PF_SD_CARD_OK && stops==1 && card_initialized);
 reset(5); len=9;
 queue[5]=0xff; queue[6]=0xff; queue[7]=0xff; queue[8]=0x0b;
 assert(pf_sd_spi_write_blocks(0,data,2)==PF_SD_CARD_IO_ERROR && stops==1 && card_initialized);
 reset(0x0b); len=6; queue[5]=0xff; busy=true;
 assert(pf_sd_spi_write_blocks(0,data,2)==PF_SD_CARD_IO_ERROR && !card_initialized && !selected);
 reset(5); len=9;
 queue[5]=0xff; queue[6]=0xff; queue[7]=0xff; queue[8]=5;
 short_exchange=10; /* CMD25 stop token transfer fails after both blocks. */
 assert(pf_sd_spi_write_blocks(0,data,2)==PF_SD_CARD_IO_ERROR && !card_initialized);
 reset(5); short_exchange=0;
 assert(pf_sd_spi_sync()==PF_SD_CARD_IO_ERROR && !card_initialized);
 reset(5); len=1; queue[0]=0; busy=true;
 assert(pf_sd_spi_sync()==PF_SD_CARD_IO_ERROR && !card_initialized);
 assert(pf_sd_spi_sync()==PF_SD_CARD_NOT_READY);
 puts("Complete production SPI write regression passed");
}
