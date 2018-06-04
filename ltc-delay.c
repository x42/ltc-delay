/* JACK Linear Time Code delay-measurement
 * Copyright (C) 2012-2018 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <getopt.h>
#include <jack/jack.h>
#include <jack/ringbuffer.h>
#include <ltc.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32
#include <signal.h>
#endif

static jack_client_t*     j_client      = NULL;
static jack_port_t*       j_output_port = NULL;
static jack_port_t*       j_input_port  = NULL;
static jack_ringbuffer_t* j_rb          = NULL;
static jack_nframes_t     j_samplerate  = 48000;

static LTCEncoder* encoder = NULL;
static LTCDecoder* decoder = NULL;

static unsigned long int monotonic_cnt = 0;
static unsigned int      fps           = 25; // 24, 25 or 30
static float             volume_dbfs   = -6.0;

pthread_mutex_t ltc_thread_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready      = PTHREAD_COND_INITIALIZER;

static int active = 0; // 0: starting, 1:running, 2:shutdown
static int debug  = 0;

static int
process (jack_nframes_t n_samples, void* arg)
{
	jack_default_audio_sample_t* in  = jack_port_get_buffer (j_input_port, n_samples);
	jack_default_audio_sample_t* out = jack_port_get_buffer (j_output_port, n_samples);

	if (active != 1) {
		memset (out, 0, sizeof (jack_default_audio_sample_t) * n_samples);
		return 0;
	}

	ltc_decoder_write_float (decoder, in, n_samples, monotonic_cnt);

	if (jack_ringbuffer_read_space (j_rb) > sizeof (jack_default_audio_sample_t) * n_samples) {
		jack_ringbuffer_read (j_rb, (void*)out, sizeof (jack_default_audio_sample_t) * n_samples);
		monotonic_cnt += n_samples;
	} else {
		memset (out, 0, sizeof (jack_default_audio_sample_t) * n_samples);
	}

	if (pthread_mutex_trylock (&ltc_thread_lock) == 0) {
		pthread_cond_signal (&data_ready);
		pthread_mutex_unlock (&ltc_thread_lock);
	}
	return 0;
}

static void
cleanup (int term)
{
	if (j_client) {
		jack_deactivate (j_client);
		jack_client_close (j_client);
	}

	if (j_rb) {
		jack_ringbuffer_free (j_rb);
	}

	ltc_encoder_free (encoder);
	ltc_decoder_free (decoder);

	if (term) {
		printf ("bye.\n");
		exit (term);
	}

	j_client = NULL;
	j_rb     = NULL;
	encoder  = NULL;
	decoder  = NULL;
}

static void
jack_shutdown (void* arg)
{
	fprintf (stderr, "recv. shutdown request from jackd.\n");
	active = 2;
	pthread_cond_signal (&data_ready);
}

static void
init_jack ()
{
	jack_status_t status;
	j_client = jack_client_open ("ltcdelay", JackNoStartServer, &status);

	if (j_client == NULL) {
		fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) {
			fprintf (stderr, "Error: Unable to connect to JACK server\n");
		}
		exit (1);
	}

	jack_set_process_callback (j_client, process, 0);
	jack_on_shutdown (j_client, jack_shutdown, 0);

	j_samplerate = jack_get_sample_rate (j_client);

	if ((j_output_port = jack_port_register (j_client, "out", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0)) == 0) {
		fprintf (stderr, "Error: Cannot register jack output port.\n");
		cleanup (1);
	}

	if ((j_input_port = jack_port_register (j_client, "in", JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)) == 0) {
		fprintf (stderr, "Error: Cannot register jack input port.\n");
		cleanup (1);
	}

	const size_t rbsize = j_samplerate * sizeof (jack_default_audio_sample_t);
	j_rb                = jack_ringbuffer_create (rbsize);
	jack_ringbuffer_mlock (j_rb);
	memset (j_rb->buf, 0, rbsize);

	if (jack_activate (j_client)) {
		fprintf (stderr, "Error: Cannot activate client");
		cleanup (1);
	}
}

static void
main_loop (void)
{
	/* default range from libltc (38..218) || - 128.0  -> (-90..90) */
	const float        smult      = pow (10, volume_dbfs / 20.0) / 90.0;
	const unsigned int precache   = j_samplerate / 2;
	const unsigned int wraparound = 86400 * j_samplerate / fps; // 24h
	ltcsnd_sample_t*   enc_buf    = calloc (ltc_encoder_get_buffersize (encoder), sizeof (ltcsnd_sample_t));

	pthread_mutex_lock (&ltc_thread_lock);
	active = 1;

	double       avg_delta = 0;
	unsigned int avg_count = 0;

	unsigned long int last_signal      = 0;
	unsigned long int last_notify_time = 0;

	const unsigned long int notify_dt = j_samplerate / 2;

	while (active == 1) {
		while (jack_ringbuffer_read_space (j_rb) < (precache * sizeof (jack_default_audio_sample_t))) {
			int byteCnt;
			for (byteCnt = 0; byteCnt < 10; byteCnt++) {
				int i;
				ltc_encoder_encode_byte (encoder, byteCnt, 1.0);
				const int len = ltc_encoder_get_buffer (encoder, enc_buf);
				for (i = 0; i < len; i++) {
					const float v1 = enc_buf[i] - 128;

					jack_default_audio_sample_t val = (jack_default_audio_sample_t) (v1 * smult);

					if (jack_ringbuffer_write (j_rb, (void*)&val, sizeof (jack_default_audio_sample_t)) != sizeof (jack_default_audio_sample_t)) {
						fprintf (stderr, "ERROR: ringbuffer overflow\n");
					}
				}
			}
			ltc_encoder_inc_timecode (encoder);
		}

		int i;
		int frames_in_queue = ltc_decoder_queue_length (decoder);

		const unsigned long int now = monotonic_cnt; // volatile

		for (i = 0; i < frames_in_queue; i++) {
			LTCFrameExt   frame;
			SMPTETimecode stime;
			ltc_decoder_read (decoder, &frame);
			ltc_frame_to_time (&stime, &frame.ltc, 0);

			unsigned long int spos = stime.frame + fps * (stime.hours * 3600 + stime.mins * 60 + stime.secs);
			spos *= j_samplerate / (double)fps;

			long int delta = (frame.off_start % wraparound) - spos;

			if (delta > 0 && delta < j_samplerate) {
				avg_delta += delta;
				++avg_count;
				last_signal = now;
			}

			if (debug) {
				printf ("%02d:%02d:%02d%c%02d | %8lld %8lld%s | %.1fdB | %ld\n",
				        stime.hours,
				        stime.mins,
				        stime.secs,
				        (frame.ltc.dfbit) ? '.' : ':',
				        stime.frame,
				        frame.off_start,
				        frame.off_end,
				        frame.reverse ? " R" : "  ",
				        frame.volume,
				        delta);
			}
		}

		if (now > last_notify_time + notify_dt) {
			last_notify_time = now;
			if (now - last_signal > 3 * j_samplerate) {
				avg_delta = 0;
				avg_count = 0;
			}
			if (avg_count > 0) {
				printf ("Delay %.0f\n", avg_delta / avg_count);
			} else {
				printf (" -- no recent signal\n");
			}
		}

		if (active != 1) {
			break;
		}

		pthread_cond_wait (&data_ready, &ltc_thread_lock);
	}

	free (enc_buf);
	pthread_mutex_unlock (&ltc_thread_lock);
}

static void
handle_signal (int sig)
{
	active = 2;
	pthread_cond_signal (&data_ready);
}

static struct option const long_options[] =
    {
      { "help", no_argument, 0, 'h' },
      { "version", no_argument, 0, 'V' },
      { "volume", required_argument, 0, 'l' },
      { NULL, 0, NULL, 0 }
    };

static void
usage (int status)
{
	printf ("ltc-delay - JACK audio client to measure delay.\n");
	printf ("Usage: ltc-delay [OPTION] [JACK-PORT-TO-CONNECT]*\n");
	printf ("\n"
	        "Options:\n"
	        " -h, --help             display this help and exit\n"
	        " -i, --input <port>     connect input port (default: none)\n"
	        " -l, --level <dBFS>     set output level in dBFS (default -6dBFS)\n"
	        " -o, --output <port>    connect output port (default: none)\n"
	        " -V, --version          print version information and exit\n"
	        "\n"
	        "\n"
	        "Report bugs to <robin@gareus.org>.\n"
	        "Website and manual: <https://github.com/x42/ltc-delay>\n"
	        "\n");
	exit (status);
}

int
main (int argc, char** argv)
{
	int   c;
	char* input_port  = NULL;
	char* output_port = NULL;

	while ((c = getopt_long (argc, argv,
	                         "d"  /* debug-print */
	                         "h"  /* help */
	                         "i:" /* input_port */
	                         "l:" /* loudnless/level */
	                         "o:" /* output_port */
	                         "V"  /* version */
	                         ,
	                         long_options,
	                         (int*)0)) != EOF) {
		switch (c) {
			case 'V':
				printf ("ltc-delay %s\n\n", VERSION);
				printf (
				    "Copyright (C) 2018 Robin Gareus <robin@gareus.org>\n"
				    "This is free software; see the source for copying conditions.  There is NO\n"
				    "warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n\n");
				exit (0);

			case 'h':
				usage (0);

			case 'l':
				volume_dbfs = atof (optarg);
				if (volume_dbfs > 0)
					volume_dbfs = 0;
				if (volume_dbfs < -192.0)
					volume_dbfs = -192.0;
				printf ("Output volume %.2f dBfs\n", volume_dbfs);
				break;

			case 'd':
				debug = 1;
				break;

			case 'i':
				input_port = optarg;
				break;

			case 'o':
				output_port = optarg;
				break;

			default:
				usage (EXIT_FAILURE);
		}
	}

	init_jack ();

	if (input_port) {
		if (jack_connect (j_client, input_port, jack_port_name (j_input_port))) {
			fprintf (stderr, "Warning: Cannot connect port '%s' to '%s'\n", input_port, jack_port_name (j_input_port));
		}
	}

	if (output_port) {
		if (jack_connect (j_client, jack_port_name (j_output_port), output_port)) {
			fprintf (stderr, "Warning: Cannot connect port '%s' to '%s'\n", jack_port_name (j_output_port), output_port);
		}
	}

	encoder = ltc_encoder_create (j_samplerate, fps, LTC_TV_FILM_24 /* no offset */, 0);
	decoder = ltc_decoder_create (j_samplerate / fps, 12);

#ifndef WIN32
	signal (SIGINT, handle_signal);
#endif

	main_loop ();
	cleanup (0);
	printf ("ciao.\n");
	return (0);
}
