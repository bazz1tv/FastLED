#pragma once
#pragma message "ESP32 Hardware SPI support w/ DMA added"

#include "driver/spi_master.h"

FASTLED_NAMESPACE_BEGIN

/*
 * ESP32 Hardware SPI Driver w/ DMA
 *
 * Copyright (c) 2023 Bazz1TV
 */

#ifndef FASTLED_ESP32_SPI_BUS
    #define FASTLED_ESP32_SPI_BUS VSPI
#endif

#if FASTLED_ESP32_SPI_BUS == VSPI
    static uint8_t spiClk = 18;
    static uint8_t spiMiso = 19;
    static uint8_t spiMosi = 23;
    static uint8_t spiCs = 5;
    #define FASTLED_ESP32_SPI_HOST VSPI_HOST
#elif FASTLED_ESP32_SPI_BUS == HSPI
    static uint8_t spiClk = 14;
    static uint8_t spiMiso = 12;
    static uint8_t spiMosi = 13;
    static uint8_t spiCs = 15;
    #define FASTLED_ESP32_SPI_HOST HSPI_HOST
#elif FASTLED_ESP32_SPI_BUS == FSPI  // ESP32S2 can re-route to arbitrary pins
    #define spiMosi DATA_PIN
    #define spiClk CLOCK_PIN
    #define spiMiso -1
    #define spiCs -1
    #define FASTLED_ESP32_SPI_HOST HSPI_HOST
#endif

#define NUM_DMA_BUFS 2
#define DMA_BUF_SIZE 4092

static uint8_t *dmaBuf[NUM_DMA_BUFS];
static int bufidx;
static const constexpr char *FTAG = "FastLED Bazz";
static spi_device_handle_t spi;
static int sending_line;
static int calc_line;

template <uint8_t DATA_PIN, uint8_t CLOCK_PIN, uint32_t SPI_SPEED>
class ESP32SPIOutput {
	Selectable 	*m_pSelect;
    


public:
	// Verify that the pins are valid
	static_assert(FastPin<DATA_PIN>::validpin(), "Invalid data pin specified");
	static_assert(FastPin<CLOCK_PIN>::validpin(), "Invalid clock pin specified");

	ESP32SPIOutput() { m_pSelect = NULL; }
	ESP32SPIOutput(Selectable *pSelect) { m_pSelect = pSelect; }
	void setSelect(Selectable *pSelect) { m_pSelect = pSelect; }

	void init() {
        ESP_LOGW(FTAG, "INIT");
        sending_line = -1;
        calc_line = 0;
        spi_bus_config_t buscfg = {
            .mosi_io_num = spiMosi,
            .miso_io_num = spiMiso,
            .sclk_io_num = spiClk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = DMA_BUF_SIZE,
            .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_IOMUX_PINS,
        };

        spi_device_interface_config_t devcfg {
            .command_bits = 0,  // 0-16
            .address_bits = 0,  // 0-64
            .dummy_bits = 0,
            .mode = SPI_MODE0,
            .duty_cycle_pos = 128,  // default: 128
            .cs_ena_pretrans = 2,   // only for half-duplex
            .cs_ena_posttrans = 2,
            .clock_speed_hz = SPI_SPEED,
            .input_delay_ns = 0,
            .spics_io_num = spiCs,  // VSPI
            .flags = SPI_DEVICE_NO_DUMMY | SPI_DEVICE_HALFDUPLEX,
            .queue_size = NUM_DMA_BUFS,
            .pre_cb = NULL,
            .post_cb = NULL,
        };

        esp_err_t ret;
        ret = spi_bus_initialize(FASTLED_ESP32_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
        ESP_ERROR_CHECK(ret);
        ret = spi_bus_add_device(FASTLED_ESP32_SPI_HOST, &devcfg, &spi);
        ESP_ERROR_CHECK(ret);
        ret = spi_device_acquire_bus(spi, portMAX_DELAY);
        ESP_ERROR_CHECK(ret);
        
        //spi_device_release_bus()
        // probably wont be needed
        //spi_bus_remove_device()
        //spi_bus_free()

        for (int i=0; i < NUM_DMA_BUFS; i++)
        {
            do {
                if (DMA_BUF_SIZE % 4 != 0) {
                    ESP_LOGW(FTAG, "[WARN] DMA buffer size (%d) is not a multiple of 4 bytes\n", DMA_BUF_SIZE);
                }
                dmaBuf[i] = (uint8_t *) heap_caps_malloc(DMA_BUF_SIZE * sizeof(uint8_t), MALLOC_CAP_DMA);
                if (dmaBuf[i] == NULL)  ESP_LOGW(FTAG, "Could not allocate enough DMA memory!");
            } while (dmaBuf[i] == NULL);
        }

        //USEFUL STUFF
	}

    void send_line_finish()
    {
        //ESP_LOGW(FTAG, "send_line_finish()");
        spi_transaction_t *rtrans;
        esp_err_t ret;
        //Wait for all 6 transactions to be done and get back the results.
        ret=spi_device_get_trans_result(spi, &rtrans, portMAX_DELAY);
        assert(ret==ESP_OK);
    }


	// stop the SPI output.  Pretty much a NOP with software, as there's no registers to kick
	static void stop() { }

	// wait until the SPI subsystem is ready for more data to write.  A NOP when bitbanging
	static void wait() __attribute__((always_inline)) { }
	static void waitFully() __attribute__((always_inline)) { wait(); }

	static void writeByteNoWait(uint8_t b) __attribute__((always_inline)) { writeByte(b); }
	static void writeBytePostWait(uint8_t b) __attribute__((always_inline)) { writeByte(b); wait(); }

	static void writeWord(uint16_t w) __attribute__((always_inline)) { writeByte(w>>8); writeByte(w&0xFF); }

	// naive writeByte implelentation, simply calls writeBit on the 8 bits in the byte.
	static void writeByte(uint8_t b) {
		dmaBuf[calc_line][bufidx++] = b;
	}

public:

	// select the SPI output (TODO: research whether this really means hi or lo.  Alt TODO: move select responsibility out of the SPI classes
	// entirely, make it up to the caller to remember to lock/select the line?)
	void select() {
        bufidx = 0;
	} 

	// release the SPI line
	void release() {
        esp_err_t ret;
		static spi_transaction_t trans[NUM_DMA_BUFS];
        
        memset(&trans[calc_line], 0, sizeof(spi_transaction_t));
        trans[calc_line].length = bufidx * 8; // length in bits
        assert((bufidx) <= DMA_BUF_SIZE);
        trans[calc_line].tx_buffer = dmaBuf[calc_line];
        ret = spi_device_queue_trans(spi, &trans[calc_line], portMAX_DELAY);

        if (sending_line != -1)
            send_line_finish();
        sending_line = calc_line;
        calc_line = (calc_line + 1) % NUM_DMA_BUFS;
	}

	// Write out len bytes of the given value out over ledSPI.  Useful for quickly flushing, say, a line of 0's down the line.
	void writeBytesValue(uint8_t value, int len) {
		/*select();
		writeBytesValueRaw(value, len);
		release();*/
	}

	static void writeBytesValueRaw(uint8_t value, int len) {
		/*while(len--) {
			ledSPI.transfer(value); 
		}*/
	}

	// write a block of len uint8_ts out.  Need to type this better so that explicit casts into the call aren't required.
	// note that this template version takes a class parameter for a per-byte modifier to the data.
	template <class D> void writeBytes(REGISTER uint8_t *data, int len) {
		/*select();
		uint8_t *end = data + len;
		while(data != end) {
			writeByte(D::adjust(*data++));
		}
		D::postBlock(len);
		release();*/
	}

	// default version of writing a block of data out to the SPI port, with no data modifications being made
	void writeBytes(REGISTER uint8_t *data, int len) { writeBytes<DATA_NOP>(data, len); }

	// write a single bit out, which bit from the passed in byte is determined by template parameter
	template <uint8_t BIT> inline void writeBit(uint8_t b) {
		//ledSPI.transfer(b);
	}

	// write a block of uint8_ts out in groups of three.  len is the total number of uint8_ts to write out.  The template
	// parameters indicate how many uint8_ts to skip at the beginning of each grouping, as well as a class specifying a per
	// byte of data modification to be made.  (See DATA_NOP above)
	template <uint8_t FLAGS, class D, EOrder RGB_ORDER>  __attribute__((noinline)) void writePixels(PixelController<RGB_ORDER> pixels) {
		/*select();
		int len = pixels.mLen;
		while(pixels.has(1)) {
			if(FLAGS & FLAG_START_BIT) {
				writeBit<0>(1);
			}
			writeByte(D::adjust(pixels.loadAndScale0()));
			writeByte(D::adjust(pixels.loadAndScale1()));
			writeByte(D::adjust(pixels.loadAndScale2()));
			pixels.advanceData();
			pixels.stepDithering();
		}
		D::postBlock(len);
		release();*/
	}
};

FASTLED_NAMESPACE_END
