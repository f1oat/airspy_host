/*
 * Copyright 2012 Jared Boone <jared@sharebrained.com>
 * Copyright 2014 Benjamin Vernoux <bvernoux@airspy.com>
 *
 * This file is part of AirSpy (based on HackRF project).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <airspy.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#define AIRSPY_RX_VERSION "1.0.0 RC2 26 Nov 2014"

#ifndef bool
typedef int bool;
#define true 1
#define false 0
#endif

#ifdef _WIN32
#include <windows.h>

#ifdef _MSC_VER

#ifdef _WIN64
typedef int64_t ssize_t;
#else
typedef int32_t ssize_t;
#endif

#define strtoull _strtoui64
#define snprintf _snprintf

int gettimeofday(struct timeval *tv, void* ignored)
{
	FILETIME ft;
	unsigned __int64 tmp = 0;
	if (NULL != tv) {
		GetSystemTimeAsFileTime(&ft);
		tmp |= ft.dwHighDateTime;
		tmp <<= 32;
		tmp |= ft.dwLowDateTime;
		tmp /= 10;
		tmp -= 11644473600000000Ui64;
		tv->tv_sec = (long)(tmp / 1000000UL);
		tv->tv_usec = (long)(tmp % 1000000UL);
	}
	return 0;
}

#endif
#endif

#if defined(__GNUC__)
#include <unistd.h>
#include <sys/time.h>
#endif

#include <signal.h>

#if defined _WIN32
	#define sleep(a) Sleep( (a*1000) )
#endif

#define SAMPLE_SCALE_FLOAT_TO_INT ( (8192.0f) )

#define FLOAT32_EL_SIZE_BYTE (4)	/* 4bytes = 32bit float */
#define INT16_EL_SIZE_BYTE (2)   /* 2bytes = 16bit int */

#define FD_BUFFER_SIZE (16*1024)

#define FREQ_ONE_MHZ (1000000ul)
#define FREQ_ONE_MHZ_U64 (1000000ull)

#define DEFAULT_SAMPLE_RATE_HZ (10000000) /* 10MHz airspy sample rate */
#define DEFAULT_SAMPLE_RATE (AIRSPY_SAMPLERATE_10MSPS)
#define DEFAULT_SAMPLE_TYPE (AIRSPY_SAMPLE_INT16_IQ)

#define DEFAULT_FREQ_HZ (900000000ul) /* 900MHz */

#define DEFAULT_VGA_IF_GAIN (1)
#define DEFAULT_LNA_GAIN (8)
#define DEFAULT_MIXER_GAIN (8)

#define FREQ_HZ_MIN (24000000ul) /* 24MHz */
#define FREQ_HZ_MAX (1900000000ul) /* 1900MHz (officially 1750MHz) */
#define SAMPLE_RATE_MAX (AIRSPY_SAMPLERATE_END-1)
#define SAMPLE_TYPE_MAX (AIRSPY_SAMPLE_END-1)
#define BIAST_MAX (1)
#define VGA_GAIN_MAX (15)
#define MIXER_GAIN_MAX (15)
#define LNA_GAIN_MAX (14)
#define SAMPLES_TO_XFER_MAX_U64 (0x8000000000000000ull) /* Max value */

/* WAVE or RIFF WAVE file format containing data for AirSpy compatible with SDR# Wav IQ file */
typedef struct 
{
		char groupID[4]; /* 'RIFF' */
		uint32_t size; /* File size + 8bytes */
		char riffType[4]; /* 'WAVE'*/
} t_WAVRIFF_hdr;

#define FormatID "fmt "   /* chunkID for Format Chunk. NOTE: There is a space at the end of this ID. */

typedef struct {
	char chunkID[4]; /* 'fmt ' */
	uint32_t chunkSize; /* 16 fixed */

	uint16_t wFormatTag; /* 1=PCM8/16, 3=Float32 */
	uint16_t wChannels;
	uint32_t dwSamplesPerSec; /* Freq Hz sampling */
	uint32_t dwAvgBytesPerSec; /* Freq Hz sampling x 2 */
	uint16_t wBlockAlign;
	uint16_t wBitsPerSample;
} t_FormatChunk;

typedef struct 
{
		char chunkID[4]; /* 'data' */
		uint32_t chunkSize; /* Size of data in bytes */
	/* For IQ samples I(16 or 32bits) then Q(16 or 32bits), I, Q ... */
} t_DataChunk;

typedef struct
{
	t_WAVRIFF_hdr hdr;
	t_FormatChunk fmt_chunk;
	t_DataChunk data_chunk;
} t_wav_file_hdr;

t_wav_file_hdr wave_file_hdr = 
{
	/* t_WAVRIFF_hdr */
	{
		{ 'R', 'I', 'F', 'F' }, /* groupID */
		0, /* size to update later */
		{ 'W', 'A', 'V', 'E' }
	},
	/* t_FormatChunk */
	{
		{ 'f', 'm', 't', ' ' }, /* char		chunkID[4];  */
		16, /* uint32_t chunkSize; */
		0, /* uint16_t wFormatTag; to update later */
		0, /* uint16_t wChannels; to update later */
		0, /* uint32_t dwSamplesPerSec; Freq Hz sampling to update later */
		0, /* uint32_t dwAvgBytesPerSec; to update later */
		0, /* uint16_t wBlockAlign; to update later */
		0, /* uint16_t wBitsPerSample; to update later  */
	},
	/* t_DataChunk */
	{
		{ 'd', 'a', 't', 'a' }, /* char chunkID[4]; */
		0, /* uint32_t	chunkSize; to update later */
	}
};

#define U64TOA_MAX_DIGIT (31)
typedef struct 
{
		char data[U64TOA_MAX_DIGIT+1];
} t_u64toa;

receiver_mode_t receiver_mode = RECEIVER_MODE_RX;

unsigned int vga_gain = DEFAULT_VGA_IF_GAIN;
unsigned int lna_gain = DEFAULT_LNA_GAIN;
unsigned int mixer_gain = DEFAULT_MIXER_GAIN;

/* WAV default values */
uint16_t wav_format_tag=1; /* PCM8 or PCM16 */
uint16_t wav_nb_channels=2;
uint32_t wav_sample_per_sec = DEFAULT_SAMPLE_RATE_HZ;
uint16_t wav_nb_byte_per_sample=2;
uint16_t wav_nb_bits_per_sample=16;

airspy_read_partid_serialno_t read_partid_serialno;

volatile bool do_exit = false;

FILE* fd = NULL;
volatile uint32_t byte_count = 0;

bool receive = false;
bool receive_wav = false;

struct timeval time_start;
struct timeval t_start;
	
bool freq = false;
uint32_t freq_hz;

bool limit_num_samples = false;
uint64_t samples_to_xfer = 0;
uint64_t bytes_to_xfer = 0;

bool sample_rate = false;
airspy_samplerate_t sample_rate_val;

bool sample_type = false;
enum airspy_sample_type sample_type_val = AIRSPY_SAMPLE_INT16_IQ;

bool biast = false;
uint32_t biast_val;

bool serial_number = false;
uint64_t serial_number_val;

static float
TimevalDiff(const struct timeval *a, const struct timeval *b)
{
	return (a->tv_sec - b->tv_sec) + 1e-6f * (a->tv_usec - b->tv_usec);
}

int parse_u64(char* s, uint64_t* const value) {
	uint_fast8_t base = 10;
	char* s_end;
	uint64_t u64_value;

	if( strlen(s) > 2 ) {
		if( s[0] == '0' ) {
			if( (s[1] == 'x') || (s[1] == 'X') ) {
				base = 16;
				s += 2;
			} else if( (s[1] == 'b') || (s[1] == 'B') ) {
				base = 2;
				s += 2;
			}
		}
	}

	s_end = s;
	u64_value = strtoull(s, &s_end, base);
	if( (s != s_end) && (*s_end == 0) ) {
		*value = u64_value;
		return AIRSPY_SUCCESS;
	} else {
		return AIRSPY_ERROR_INVALID_PARAM;
	}
}

int parse_u32(char* s, uint32_t* const value)
{
	uint_fast8_t base = 10;
	char* s_end;
	uint64_t ulong_value;

	if( strlen(s) > 2 ) {
		if( s[0] == '0' ) {
			if( (s[1] == 'x') || (s[1] == 'X') ) {
				base = 16;
				s += 2;
			} else if( (s[1] == 'b') || (s[1] == 'B') ) {
				base = 2;
				s += 2;
			}
		}
	}

	s_end = s;
	ulong_value = strtoul(s, &s_end, base);
	if( (s != s_end) && (*s_end == 0) ) {
		*value = (uint32_t)ulong_value;
		return AIRSPY_SUCCESS;
	} else {
		return AIRSPY_ERROR_INVALID_PARAM;
	}
}

static char *stringrev(char *str)
{
	char *p1, *p2;

	if(! str || ! *str)
		return str;

	for(p1 = str, p2 = str + strlen(str) - 1; p2 > p1; ++p1, --p2)
	{
		*p1 ^= *p2;
		*p2 ^= *p1;
		*p1 ^= *p2;
	}
	return str;
}

char* u64toa(uint64_t val, t_u64toa* str)
{
	#define BASE (10ull) /* Base10 by default */
	uint64_t sum;
	int pos;
	int digit;
	int max_len;
	char* res;

	sum = val;
	max_len = U64TOA_MAX_DIGIT;
	pos = 0;

	do
	{
		digit = (sum % BASE);
		str->data[pos] = digit + '0';
		pos++;

		sum /= BASE;
	}while( (sum>0) && (pos < max_len) );

	if( (pos == max_len) && (sum>0) )
		return NULL;

	str->data[pos] = '\0';
	res = stringrev(str->data);

	return res;
}

int rx_callback(airspy_transfer_t* transfer)
{
	uint32_t bytes_to_write;
	uint32_t nb_data;
	void* pt_rx_buffer;
	ssize_t bytes_written;

	if( fd != NULL ) 
	{
		switch(sample_type_val)
		{
			case AIRSPY_SAMPLE_FLOAT32_IQ:
				nb_data = transfer->sample_count * FLOAT32_EL_SIZE_BYTE * 2;
				byte_count += nb_data;
				bytes_to_write = nb_data;
				pt_rx_buffer = transfer->samples;
			break;

			case AIRSPY_SAMPLE_FLOAT32_REAL:
				nb_data = transfer->sample_count * FLOAT32_EL_SIZE_BYTE * 1;
				byte_count += nb_data;
				bytes_to_write = nb_data;
				pt_rx_buffer = transfer->samples;
			break;

			case AIRSPY_SAMPLE_INT16_IQ:
				nb_data = transfer->sample_count * INT16_EL_SIZE_BYTE * 2;
				byte_count += nb_data;
				bytes_to_write = nb_data;
				pt_rx_buffer = transfer->samples;
			break;

			case AIRSPY_SAMPLE_INT16_REAL:
				nb_data = transfer->sample_count * INT16_EL_SIZE_BYTE * 1;
				byte_count += nb_data;
				bytes_to_write = nb_data;
				pt_rx_buffer = transfer->samples;
			break;

			case AIRSPY_SAMPLE_UINT16_REAL:
				nb_data = transfer->sample_count * INT16_EL_SIZE_BYTE * 1;
				byte_count += nb_data;
				bytes_to_write = nb_data;
				pt_rx_buffer = transfer->samples;
			break;

			default:
				bytes_to_write = 0;
				pt_rx_buffer = NULL;
			break;
		}

		if (limit_num_samples) {
			if (bytes_to_write >= bytes_to_xfer) {
				bytes_to_write = (int)bytes_to_xfer;
			}
			bytes_to_xfer -= bytes_to_write;
		}

		if(pt_rx_buffer != NULL)
		{
			bytes_written = fwrite(pt_rx_buffer, 1, bytes_to_write, fd);
		}else
		{
			bytes_written = 0;
		}
		if ( (bytes_written != bytes_to_write) || 
				 ((limit_num_samples == true) && (bytes_to_xfer == 0)) 
				)
			return -1;
		else
			return 0;
	}else
	{
		return -1;
	}
}

static void usage(void)
{
	printf("airspy_rx v%s\n", AIRSPY_RX_VERSION);
	printf("Usage:\n");
	printf("-r <filename>: Receive data into file\n");
	printf("-w Receive data into file with WAV header and automatic name\n");
	printf(" This is for SDR# compatibility and may not work with other software\n");
	printf("[-s serial_number_64bits]: Open board with specified 64bits serial number\n");
	printf("[-f frequency_MHz]: Set frequency in MHz between [%lu, %lu] (default %luMHz)\n",
		FREQ_HZ_MIN / FREQ_ONE_MHZ, FREQ_HZ_MAX / FREQ_ONE_MHZ, DEFAULT_FREQ_HZ / FREQ_ONE_MHZ);
	printf("[-a sample_rate]: Set sample rate, 0=10MSPS(default), 1=2.5MSPS\n");
	printf("[-t sample_type]: Set sample type, \n");
	printf(" 0=FLOAT32_IQ, 1=FLOAT32_REAL, 2=INT16_IQ(default), 3=INT16_REAL, 4=U16_RAW\n");
	printf("[-b biast]: Set Bias Tee, 1=enabled, 0=disabled(default)\n");
	printf("[-v vga_gain]: Set VGA/IF gain, 0-%d (default %d)\n", VGA_GAIN_MAX, vga_gain);
	printf("[-m mixer_gain]: Set Mixer gain, 0-%d (default %d)\n", MIXER_GAIN_MAX, mixer_gain);
	printf("[-l lna_gain]: Set LNA gain, 0-%d (default %d)\n", LNA_GAIN_MAX, lna_gain);
	printf("[-n num_samples]: Number of samples to transfer (default is unlimited)\n");
}

struct airspy_device* device = NULL;

#ifdef _MSC_VER
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stdout, "Caught signal %d\n", signum);
		do_exit = true;
		return TRUE;
	}
	return FALSE;
}
#else
void sigint_callback_handler(int signum) 
{
	fprintf(stdout, "Caught signal %d\n", signum);
	do_exit = true;
}
#endif

#define PATH_FILE_MAX_LEN (FILENAME_MAX)
#define DATE_TIME_MAX_LEN (32)

int main(int argc, char** argv)
{
	int opt;
	char path_file[PATH_FILE_MAX_LEN];
	char date_time[DATE_TIME_MAX_LEN];
	t_u64toa ascii_u64_data1;
	t_u64toa ascii_u64_data2;
	const char* path = NULL;
	int result;
	time_t rawtime;
	struct tm * timeinfo;
	uint32_t file_pos;
	int exit_code = EXIT_SUCCESS;
	struct timeval t_end;
	float time_diff;
	uint32_t sample_rate_u32;
	uint32_t sample_type_u32;
	double freq_hz_temp;

	while( (opt = getopt(argc, argv, "r:ws:f:a:t:b:v:m:l:n:")) != EOF )
	{
		result = AIRSPY_SUCCESS;
		switch( opt ) 
		{
			case 'r':
				receive = true;
				path = optarg;
			break;

			case 'w':
				receive_wav = true;
			 break;

			case 's':
				serial_number = true;
				result = parse_u64(optarg, &serial_number_val);
			break;

			case 'f':
				freq = true;
				freq_hz_temp = strtod(optarg, NULL) * (double)FREQ_ONE_MHZ;
				if(freq_hz_temp <= (double)FREQ_HZ_MAX)
					freq_hz = (uint32_t)freq_hz_temp;
				else
					freq_hz = UINT_MAX;
			break;

			case 'a': /* Sample rate see also airspy_samplerate_t */
				result = parse_u32(optarg, &sample_rate_u32);
				switch (sample_rate_u32)
				{
					case 0:
						sample_rate_val = AIRSPY_SAMPLERATE_10MSPS;
						wav_sample_per_sec = 10000000;
					break;

					case 1:
						sample_rate_val = AIRSPY_SAMPLERATE_2_5MSPS;
						wav_sample_per_sec = 2500000;
					break;

					default:
						/* Invalid value will display error */
						sample_rate_val = SAMPLE_RATE_MAX+1;
					break;
				}
			break;

			case 't': /* Sample type see also airspy_sample_type */
				result = parse_u32(optarg, &sample_type_u32);
				switch (sample_type_u32)
				{
					case 0:
						sample_type_val = AIRSPY_SAMPLE_FLOAT32_IQ;
						wav_format_tag = 3; /* Float32 */
						wav_nb_channels = 2;
						wav_nb_bits_per_sample = 32;
						wav_nb_byte_per_sample = (wav_nb_bits_per_sample / 8);
					break;

					case 1:
						sample_type_val = AIRSPY_SAMPLE_FLOAT32_REAL;
						wav_format_tag = 3; /* Float32 */
						wav_nb_channels = 1;
						wav_nb_bits_per_sample = 32;
						wav_nb_byte_per_sample = (wav_nb_bits_per_sample / 8);
					break;

					case 2:
						sample_type_val = AIRSPY_SAMPLE_INT16_IQ;
						wav_format_tag = 1; /* PCM8 or PCM16 */
						wav_nb_channels = 2;
						wav_nb_bits_per_sample = 16;
						wav_nb_byte_per_sample = (wav_nb_bits_per_sample / 8);
					break;

					case 3:
						sample_type_val = AIRSPY_SAMPLE_INT16_REAL;
						wav_format_tag = 1; /* PCM8 or PCM16 */
						wav_nb_channels = 1;
						wav_nb_bits_per_sample = 16;
						wav_nb_byte_per_sample = (wav_nb_bits_per_sample / 8);
					break;

					case 4:
						sample_type_val = AIRSPY_SAMPLE_UINT16_REAL;
						wav_format_tag = 1; /* PCM8 or PCM16 */
						wav_nb_channels = 1;
						wav_nb_bits_per_sample = 16;
						wav_nb_byte_per_sample = (wav_nb_bits_per_sample / 8);
					break;

					default:
						/* Invalid value will display error */
						sample_type_val = SAMPLE_TYPE_MAX+1;
					break;
				}
			break;

			case 'b':
				serial_number = true;
				result = parse_u32(optarg, &biast_val);
			break;

			case 'v':
				result = parse_u32(optarg, &vga_gain);
			break;

			case 'm':
				result = parse_u32(optarg, &mixer_gain);
			break;

			case 'l':
				result = parse_u32(optarg, &lna_gain);
			break;

			case 'n':
				limit_num_samples = true;
				result = parse_u64(optarg, &samples_to_xfer);
				bytes_to_xfer = samples_to_xfer * 2;
			break;

			default:
				printf("unknown argument '-%c %s'\n", opt, optarg);
				usage();
				return EXIT_FAILURE;
		}
		
		if( result != AIRSPY_SUCCESS ) {
			printf("argument error: '-%c %s' %s (%d)\n", opt, optarg, airspy_error_name(result), result);
			usage();
			return EXIT_FAILURE;
		}		
	}

	if (samples_to_xfer >= SAMPLES_TO_XFER_MAX_U64) {
		printf("argument error: num_samples must be less than %s/%sMio\n",
				u64toa(SAMPLES_TO_XFER_MAX_U64, &ascii_u64_data1),
				u64toa((SAMPLES_TO_XFER_MAX_U64/FREQ_ONE_MHZ_U64), &ascii_u64_data2) );
		usage();
		return EXIT_FAILURE;
	}

	if( freq ) {
		if( (freq_hz >= FREQ_HZ_MAX) || (freq_hz < FREQ_HZ_MIN) )
		{
			printf("argument error: frequency_MHz=%.6f MHz and shall be between [%lu, %lu[ MHz\n",
							((double)freq_hz/(double)FREQ_ONE_MHZ), FREQ_HZ_MIN/FREQ_ONE_MHZ, FREQ_HZ_MAX/FREQ_ONE_MHZ);
			usage();
			return EXIT_FAILURE;
		}
	}else
	{
		/* Use default freq */
		freq_hz = DEFAULT_FREQ_HZ;
	}

	receiver_mode = RECEIVER_MODE_RX;
	if( receive_wav ) 
	{
		time (&rawtime);
		timeinfo = localtime (&rawtime);
		receiver_mode = RECEIVER_MODE_RX;
		/* File format AirSpy Year(2013), Month(11), Day(28), Hour Min Sec+Z, Freq kHz, IQ.wav */
		strftime(date_time, DATE_TIME_MAX_LEN, "%Y%m%d_%H%M%S", timeinfo);
		snprintf(path_file, PATH_FILE_MAX_LEN, "AirSpy_%sZ_%ukHz_IQ.wav", date_time, (uint32_t)(freq_hz/(1000ull)) );
		path = path_file;
		printf("Receive wav file: %s\n", path);
	}	

	if( path == NULL ) {
		printf("error: you shall specify at least -r <with filename> or -w option\n");
		usage();
		return EXIT_FAILURE;
	}

	if(sample_rate_val > SAMPLE_RATE_MAX) {
		printf("argument error: sample_rate out of range\n");
		usage();
		return EXIT_FAILURE;
	}

	if(sample_type_val > SAMPLE_TYPE_MAX) {
		printf("argument error: sample_type out of range\n");
		usage();
		return EXIT_FAILURE;
	}

	if(biast_val > BIAST_MAX) {
		printf("argument error: biast_val out of range\n");
		usage();
		return EXIT_FAILURE;
	}

	if(vga_gain > VGA_GAIN_MAX) {
		printf("argument error: vga_gain out of range\n");
		usage();
		return EXIT_FAILURE;
	}

	if(mixer_gain > MIXER_GAIN_MAX) {
		printf("argument error: mixer_gain out of range\n");
		usage();
		return EXIT_FAILURE;
	}

	if(lna_gain > LNA_GAIN_MAX) {
		printf("argument error: lna_gain out of range\n");
		usage();
		return EXIT_FAILURE;
	}

#ifdef DEBUG_PARAM
	{
		uint32_t serial_number_msb_val;
		uint32_t serial_number_lsb_val;
		serial_number_msb_val = (uint32_t)(serial_number_val >> 32);
		serial_number_lsb_val = (uint32_t)(serial_number_val & 0xFFFFFFFF);
		if(serial_number)
			printf("serial_number_64bits -s 0x%08X%08X\n", serial_number_msb_val, serial_number_lsb_val);
		printf("frequency_MHz -f %.6f MHz (%sHz)\n",((double)freq_hz/(double)FREQ_ONE_MHZ), u64toa(freq_hz, &ascii_u64_data1) );
		printf("sample_rate -a %d\n", sample_rate_val);
		printf("sample_type -t %d\n", sample_type_val);
		printf("biast -b %d\n", biast_val);
		printf("vga_gain -v %u\n", vga_gain);
		printf("mixer_gain -m %u\n", mixer_gain);
		printf("lna_gain -l %u\n", lna_gain);
		if( limit_num_samples ) {
			printf("num_samples -n %s (%sMio)\n",
							u64toa(samples_to_xfer, &ascii_u64_data1),
							u64toa((samples_to_xfer/FREQ_ONE_MHZ), &ascii_u64_data2));
		}
	}
#endif

	result = airspy_init();
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_init() failed: %s (%d)\n", airspy_error_name(result), result);
		return EXIT_FAILURE;
	}

	if(serial_number == true)
	{
		result = airspy_open_sn(&device, serial_number_val);
		if( result != AIRSPY_SUCCESS ) {
			printf("airspy_open_sn() failed: %s (%d)\n", airspy_error_name(result), result);
			airspy_exit();
			return EXIT_FAILURE;
		}
	}else
	{
		result = airspy_open(&device);
		if( result != AIRSPY_SUCCESS ) {
			printf("airspy_open() failed: %s (%d)\n", airspy_error_name(result), result);
			airspy_exit();
			return EXIT_FAILURE;
		}
	}
	
	result = airspy_board_partid_serialno_read(device, &read_partid_serialno);
	if (result != AIRSPY_SUCCESS) {
			fprintf(stderr, "airspy_board_partid_serialno_read() failed: %s (%d)\n",
				airspy_error_name(result), result);
			airspy_close(device);
			airspy_exit();
			return EXIT_FAILURE;
	}
	printf("Board Serial Number: 0x%08X%08X\n",
		read_partid_serialno.serial_no[2],
		read_partid_serialno.serial_no[3]);
	
	result = airspy_set_samplerate(device, sample_rate_val);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_samplerate() failed: %s (%d)\n", airspy_error_name(result), result);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}

	result = airspy_set_sample_type(device, sample_type_val);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_sample_type() failed: %s (%d)\n", airspy_error_name(result), result);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}

	result = airspy_set_rf_bias(device, biast_val);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_rf_bias() failed: %s (%d)\n", airspy_error_name(result), result);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}

	fd = fopen(path, "wb");
	if( fd == NULL ) {
		printf("Failed to open file: %s\n", path);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}
	/* Change fd buffer to have bigger one to store data to file */
	result = setvbuf(fd , NULL , _IOFBF , FD_BUFFER_SIZE);
	if( result != 0 ) {
		printf("setvbuf() failed: %d\n", result);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}
	
	/* Write Wav header */
	if( receive_wav ) 
	{
		fwrite(&wave_file_hdr, 1, sizeof(t_wav_file_hdr), fd);
	}
	
#ifdef _MSC_VER
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#else
	signal(SIGINT, &sigint_callback_handler);
	signal(SIGILL, &sigint_callback_handler);
	signal(SIGFPE, &sigint_callback_handler);
	signal(SIGSEGV, &sigint_callback_handler);
	signal(SIGTERM, &sigint_callback_handler);
	signal(SIGABRT, &sigint_callback_handler);
#endif

	result = airspy_set_vga_gain(device, vga_gain);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_vga_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	}

	result = airspy_set_mixer_gain(device, mixer_gain);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_mixer_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	}

	result = airspy_set_lna_gain(device, lna_gain);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_lna_gain() failed: %s (%d)\n", airspy_error_name(result), result);
	}

	result = airspy_start_rx(device, rx_callback, NULL);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_start_rx() failed: %s (%d)\n", airspy_error_name(result), result);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}

	result = airspy_set_freq(device, freq_hz);
	if( result != AIRSPY_SUCCESS ) {
		printf("airspy_set_freq() failed: %s (%d)\n", airspy_error_name(result), result);
		airspy_close(device);
		airspy_exit();
		return EXIT_FAILURE;
	}

	gettimeofday(&t_start, NULL);
	gettimeofday(&time_start, NULL);

	printf("Stop with Ctrl-C\n");
	while( (airspy_is_streaming(device) == AIRSPY_TRUE) &&
			(do_exit == false) ) 
	{
		uint32_t byte_count_now;
		struct timeval time_now;
		float time_difference, rate;
		sleep(1);
		
		gettimeofday(&time_now, NULL);
		
		byte_count_now = byte_count;
		byte_count = 0;
		
		time_difference = TimevalDiff(&time_now, &time_start);
		rate = (float)byte_count_now / time_difference;
		printf("%4.3f MiB / %4.1f sec = %4.3f MiB/second\n",
				(byte_count_now / 1e6f), time_difference, (rate / 1e6f) );

		time_start = time_now;

		if (byte_count_now == 0) {
			exit_code = EXIT_FAILURE;
			printf("\nCouldn't transfer any bytes for one second.\n");
			break;
		}
	}
	
	result = airspy_is_streaming(device);	
	if (do_exit)
	{
		printf("\nUser cancel, exiting...\n");
	} else {
		printf("\nExiting... airspy_is_streaming() result: %s (%d)\n", airspy_error_name(result), result);
	}
	
	gettimeofday(&t_end, NULL);
	time_diff = TimevalDiff(&t_end, &t_start);
	printf("Total time: %5.5f s\n", time_diff);
	
	if(device != NULL)
	{
		result = airspy_stop_rx(device);
		if( result != AIRSPY_SUCCESS ) {
			printf("airspy_stop_rx() failed: %s (%d)\n", airspy_error_name(result), result);
		}else {
			printf("airspy_stop_rx() done\n");
		}

		result = airspy_close(device);
		if( result != AIRSPY_SUCCESS ) 
		{
			printf("airspy_close() failed: %s (%d)\n", airspy_error_name(result), result);
		}else {
			printf("airspy_close() done\n");
		}
		
		airspy_exit();
		printf("airspy_exit() done\n");
	}
		
	if(fd != NULL)
	{
		if( receive_wav ) 
		{
			/* Get size of file */
			file_pos = ftell(fd);
			/* Wav Header */
			wave_file_hdr.hdr.size = file_pos - 8;
			/* Wav Format Chunk */
			wave_file_hdr.fmt_chunk.wFormatTag = wav_format_tag;
			wave_file_hdr.fmt_chunk.wChannels = wav_nb_channels;
			wave_file_hdr.fmt_chunk.dwSamplesPerSec = wav_sample_per_sec;
			wave_file_hdr.fmt_chunk.dwAvgBytesPerSec = wave_file_hdr.fmt_chunk.dwSamplesPerSec * wav_nb_byte_per_sample;
			wave_file_hdr.fmt_chunk.wBlockAlign = wav_nb_channels * (wav_nb_bits_per_sample / 8);
			wave_file_hdr.fmt_chunk.wBitsPerSample = wav_nb_bits_per_sample;
			/* Wav Data Chunk */
			wave_file_hdr.data_chunk.chunkSize = file_pos - sizeof(t_wav_file_hdr);
			/* Overwrite header with updated data */
			rewind(fd);
			fwrite(&wave_file_hdr, 1, sizeof(t_wav_file_hdr), fd);
		}	
		fclose(fd);
		fd = NULL;
		printf("fclose(fd) done\n");
	}
	printf("exit\n");
	return exit_code;
}
