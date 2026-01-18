//
// Created by ekeleze on 1/17/26.
//

#include <asoundlib.h>
#include <mgba/core/core.h>
#include <mgba/gba/core.h>

#include "audio.h"

#include "mgba-util/audio-buffer.h"
#include "mgba-util/audio-resampler.h"

#include <pthread.h>

#define SAMPLERATE 22050
#define FRAME_COUNT 1024

static snd_pcm_t *pcm_handle = NULL;
static struct mAudioBuffer audio_buffer;
static struct mAudioResampler resampler;
static pthread_t audio_thread;
static bool audio_running = false;

static void* audio_thread_func(void* arg) {
	struct mCore* core = (struct mCore*)arg;
	static int16_t samples[FRAME_COUNT * 2];

	printf("Audio thread started\n");

	while (audio_running) {
		mAudioResamplerProcess(&resampler);

		size_t available = mAudioBufferAvailable(&audio_buffer);
		if (available == 0) {
			usleep(1000); // 1ms
			continue;
		}

		size_t to_write = available > FRAME_COUNT ? FRAME_COUNT : available;
		size_t read = mAudioBufferRead(&audio_buffer, samples, to_write);

		if (read > 0) {
			int err = snd_pcm_writei(pcm_handle, samples, read);
			if (err < 0) {
				snd_pcm_recover(pcm_handle, err, 1);
			}
		}
	}

	printf("Audio thread stopped\n");
	return NULL;
}

void audio_init(struct mCore* core)
{
	int err;
	snd_pcm_hw_params_t *hw_params;

	printf("audio_init: Initializing ALSA at %d Hz\n", SAMPLERATE);

	err = snd_pcm_open(&pcm_handle, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0);
	if (err < 0) {
		fprintf(stderr, "ALSA open failed: %s\n", snd_strerror(err));
		return;
	}

	snd_pcm_hw_params_alloca(&hw_params);
	snd_pcm_hw_params_any(pcm_handle, hw_params);
	snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE);
	snd_pcm_hw_params_set_channels(pcm_handle, hw_params, 2);

	unsigned int sample_rate_unsigned = SAMPLERATE;
	snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &sample_rate_unsigned, 0);

	snd_pcm_uframes_t period_size = FRAME_COUNT;
	snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, 0);

	snd_pcm_uframes_t buffer_size = FRAME_COUNT * 8;
	snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_size);

	err = snd_pcm_hw_params(pcm_handle, hw_params);
	if (err < 0) {
		fprintf(stderr, "ALSA hw_params failed: %s\n", snd_strerror(err));
		snd_pcm_close(pcm_handle);
		return;
	}

	snd_pcm_prepare(pcm_handle);

	mAudioBufferInit(&audio_buffer, FRAME_COUNT * 2, 2);
	mAudioResamplerInit(&resampler, mINTERPOLATOR_SINC);

	mAudioResamplerSetSource(&resampler, core->getAudioBuffer(core), 32768, true);
	mAudioResamplerSetDestination(&resampler, &audio_buffer, SAMPLERATE);

	core->setAudioBufferSize(core, 4096);

	audio_running = true;
	pthread_create(&audio_thread, NULL, audio_thread_func, core);

	printf("ALSA initialized with resampling: 32768 Hz -> %d Hz\n", SAMPLERATE);
	printf("Audio thread initialized\n");
}

void audio_deinit() {
	if (audio_running) {
		audio_running = false;
		pthread_join(audio_thread, NULL);
	}

	if (pcm_handle) {
		snd_pcm_drain(pcm_handle);
		snd_pcm_close(pcm_handle);
		pcm_handle = NULL;
	}
}