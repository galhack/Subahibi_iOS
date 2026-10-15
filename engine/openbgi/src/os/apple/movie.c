/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * movie.c - the Apple back end's movie player (inc/bgi/os.h, "Movies"):
 *           MPEG-1 system streams with MP2 sound, the format of the games'
 *           .mpg files, decoded with pl_mpeg (third_party/pl_mpeg.h, MIT,
 *           Dominic Szablewski)
 *
 * The original builds a DirectShow graph and shows the video in a child
 * window over the picture.  Here a thread decodes the stream in real time:
 * each video frame is converted to 0x00RRGGBB for the application, which
 * draws it over the window image in the rectangle of OS_MovieStart
 * (AppleHost_LockMovieFrame), and the sound goes through a ring buffer
 * into the audio output, where AppleMovie_MixAudio resamples it to the
 * output rate and mixes it over the engine's own sound.  The stream is read
 * from a range of a file, so a movie stored in an archive (1.647) plays
 * without being extracted.
 */
#include "apple_internal.h"

#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#define PL_MPEG_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wconversion"
#endif
#include "third_party/pl_mpeg.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

pthread_mutex_t gAppleMovieLock = PTHREAD_MUTEX_INITIALIZER;

// the open movie
static plm_t* gPlm;
static int gFd = -1;          // the file holding the stream
static off_t gBase;           // the stream's first byte in it
static size_t gSize, gPos;    // the stream's length, the reading position inside it
static pthread_t gThread;
static volatile int gRun;     // the decoding thread runs
static volatile int gEnded;   // the stream reached its end
static volatile int gPlaying; // between OS_MovieStart and OS_MovieStop
static volatile float gGain = 1.0f;

// the current picture (gAppleMovieLock)
static uint32_t* gFrame;
static int gFrameW, gFrameH;
static uint32_t gFrameSerial;
static int gRectX, gRectY, gRectW, gRectH;

// the sound: interleaved stereo float at the stream's rate (gAudioLock)
#define RING_FRAMES 65536
static float gRing[RING_FRAMES * 2];
static size_t gRingRead, gRingWrite; // frames, ever increasing
static int gMovieRate;
static double gResamplePos; // the fractional reading position of the resampler
static pthread_mutex_t gAudioLock = PTHREAD_MUTEX_INITIALIZER;

// ---- reading a range of a file ---------------------------------------------------------------

static void RangeLoad(plm_buffer_t* self, void* user)
{
	size_t avail, want;
	ssize_t got;
	BGI_UNUSED(user);
	if(self->discard_read_bytes)
		plm_buffer_discard_read_bytes(self);
	avail = self->capacity - self->length;
	want = gSize - gPos < avail ? gSize - gPos : avail;
	got = want ? pread(gFd, self->bytes + self->length, want, gBase + (off_t)gPos) : 0;
	if(got <= 0)
	{
		self->has_ended = TRUE;
		return;
	}
	gPos += (size_t)got;
	self->length += (size_t)got;
}

static void RangeSeek(plm_buffer_t* self, size_t offset, void* user)
{
	BGI_UNUSED(self);
	BGI_UNUSED(user);
	gPos = offset < gSize ? offset : gSize;
}

static size_t RangeTell(plm_buffer_t* self, void* user)
{
	BGI_UNUSED(self);
	BGI_UNUSED(user);
	return gPos;
}

// ---- decoding ----------------------------------------------------------------------------------

static void OnVideo(plm_t* plm, plm_frame_t* frame, void* user)
{
	BGI_UNUSED(plm);
	BGI_UNUSED(user);
	pthread_mutex_lock(&gAppleMovieLock);
	if(!gFrame || gFrameW != (int)frame->width || gFrameH != (int)frame->height)
	{
		free(gFrame);
		gFrame = (uint32_t*)malloc((size_t)frame->width * frame->height * 4);
		gFrameW = gFrame ? (int)frame->width : 0;
		gFrameH = gFrame ? (int)frame->height : 0;
	}
	if(gFrame)
	{
		// B, G, R, A bytes: 0xAARRGGBB as little-endian words, the 0x00RRGGBB of the window image once masked
		plm_frame_to_bgra(frame, (uint8_t*)gFrame, gFrameW * 4);
		gFrameSerial++;
	}
	pthread_mutex_unlock(&gAppleMovieLock);
}

static void OnAudio(plm_t* plm, plm_samples_t* samples, void* user)
{
	unsigned i;
	BGI_UNUSED(plm);
	BGI_UNUSED(user);
	pthread_mutex_lock(&gAudioLock);
	for(i = 0; i < samples->count; i++)
	{
		if(gRingWrite - gRingRead >= RING_FRAMES)
			break; // the output fell behind by more than a second: drop rather than overwrite
		gRing[(gRingWrite % RING_FRAMES) * 2] = samples->interleaved[i * 2];
		gRing[(gRingWrite % RING_FRAMES) * 2 + 1] = samples->interleaved[i * 2 + 1];
		gRingWrite++;
	}
	pthread_mutex_unlock(&gAudioLock);
}

static double NowSeconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

// decode in real time until the end or a stop; the end reaches the engine as the graph's EC_COMPLETE
static void* DecodeThread(void* arg)
{
	double last = NowSeconds();
	BGI_UNUSED(arg);
	while(gRun && !plm_has_ended(gPlm))
	{
		double now = NowSeconds();
		double dt = now - last;
		last = now;
		if(dt > 0.25)
			dt = 0.25; // after a stall (the application in the background) catch up without a burst
		plm_decode(gPlm, dt);
		usleep(4000);
	}
	if(gRun)
	{
		gEnded = 1;
		AppleWindow_PushEvent(APPLE_EV_MOVIE, 1, 0, 0, 0);
	}
	return NULL;
}

// ---- the OS layer -------------------------------------------------------------------------------------

static void Release(void)
{
	if(gRun)
	{
		gRun = 0;
		pthread_join(gThread, NULL);
	}
	if(gPlm)
	{
		plm_destroy(gPlm);
		gPlm = NULL;
	}
	if(gFd >= 0)
	{
		close(gFd);
		gFd = -1;
	}
	pthread_mutex_lock(&gAppleMovieLock);
	free(gFrame);
	gFrame = NULL;
	gFrameW = gFrameH = 0;
	gFrameSerial++;
	pthread_mutex_unlock(&gAppleMovieLock);
	pthread_mutex_lock(&gAudioLock);
	gRingRead = gRingWrite = 0;
	gResamplePos = 0;
	pthread_mutex_unlock(&gAudioLock);
	gPlaying = 0;
	gEnded = 0;
}

int OS_MovieOpenRange(const char* path, uint32_t offset, uint32_t size)
{
	char native[1024];
	plm_buffer_t* buf;
	Release();
	OsPosix_Path(path, native, sizeof native);
	gFd = open(native, O_RDONLY);
	if(gFd < 0)
		return OS_MOVIE_FAILED;
	if(!size)
	{ // the whole file
		off_t end = lseek(gFd, 0, SEEK_END);
		size = end > 0 && end < 0xffffffffll ? (uint32_t)end : 0;
	}
	gBase = offset;
	gSize = size;
	gPos = 0;
	buf = plm_buffer_create_with_callbacks(RangeLoad, RangeSeek, RangeTell, gSize, NULL);
	gPlm = buf ? plm_create_with_buffer(buf, 1) : NULL;
	if(!gPlm || !plm_probe(gPlm, 5000 * 1024) || plm_get_num_video_streams(gPlm) < 1)
	{
		Release();
		return OS_MOVIE_FAILED;
	}
	plm_set_video_decode_callback(gPlm, OnVideo, NULL);
	plm_set_audio_decode_callback(gPlm, OnAudio, NULL);
	plm_set_audio_enabled(gPlm, plm_get_num_audio_streams(gPlm) > 0);
	plm_set_loop(gPlm, 0);
	gMovieRate = plm_get_samplerate(gPlm);
	if(gMovieRate > 0)
		plm_set_audio_lead_time(gPlm, (double)PLM_AUDIO_SAMPLES_PER_FRAME * 2 / gMovieRate);
	return plm_get_num_audio_streams(gPlm) > 0 ? OS_MOVIE_OK : OS_MOVIE_AUDIO_NOT_RENDERED;
}

int OS_MovieOpen(const char* path)
{
	return OS_MovieOpenRange(path, 0, 0);
}

int OS_MovieStart(int x, int y, int w, int h, int32_t volume, int32_t* lengthMs)
{
	double duration;
	if(!gPlm)
		return 0;
	OS_MovieSetVolume(volume);
	duration = plm_get_duration(gPlm);
	plm_rewind(gPlm);
	gPos = 0;
	if(lengthMs)
		*lengthMs = duration > 0 ? (int32_t)(duration * 1000) : 0;
	fprintf(stderr, "bgi: movie of %.1f s starts at (%d, %d) %d x %d\n", duration, x, y, w, h);
	pthread_mutex_lock(&gAppleMovieLock);
	gRectX = x;
	gRectY = y;
	gRectW = w;
	gRectH = h;
	pthread_mutex_unlock(&gAppleMovieLock);
	gEnded = 0;
	gRun = 1;
	gPlaying = 1;
	if(pthread_create(&gThread, NULL, DecodeThread, NULL) != 0)
	{
		gRun = 0;
		Release();
		return 0;
	}
	return 1;
}

void OS_MovieStop(void)
{
	Release();
}

int OS_MovieIsRunning(void)
{
	return gPlaying && !gEnded;
}

// IBasicAudio's hundredths of a decibel, -10000 (silence) .. 0
void OS_MovieSetVolume(int32_t vol)
{
	gGain = vol <= -10000 ? 0.0f : (float)pow(10.0, vol / 2000.0);
}

// ---- for the application and the audio output -----------------------------------------------------------

int AppleMovie_Lock(uint32_t lastSerial, AppleHostMovieFrame_t* out, int* playing)
{
	*playing = gPlaying;
	pthread_mutex_lock(&gAppleMovieLock);
	if(!gPlaying || !gFrame || (lastSerial && lastSerial == gFrameSerial))
	{
		pthread_mutex_unlock(&gAppleMovieLock);
		return 0;
	}
	out->pixels = gFrame;
	out->width = gFrameW;
	out->height = gFrameH;
	out->pitch = gFrameW * 4;
	out->x = gRectX;
	out->y = gRectY;
	out->w = gRectW;
	out->h = gRectH;
	out->serial = gFrameSerial;
	return 1;
}

void AppleMovie_Unlock(void)
{
	pthread_mutex_unlock(&gAppleMovieLock);
}

/* add the movie's sound to the output buffer: linear interpolation from the
 * stream's rate to the output's, the volume applied, clipped to 16 bits */
void AppleMovie_MixAudio(int16_t* samples, int frames, int rate)
{
	double step;
	float gain = gGain;
	int i;
	if(!gPlaying || gMovieRate <= 0 || rate <= 0)
		return;
	step = (double)gMovieRate / rate;
	pthread_mutex_lock(&gAudioLock);
	for(i = 0; i < frames; i++)
	{
		size_t i0 = gRingRead + (size_t)gResamplePos;
		double frac = gResamplePos - (double)(size_t)gResamplePos;
		int c;
		if(i0 + 1 >= gRingWrite)
			break; // nothing decoded yet (or the end): silence
		for(c = 0; c < 2; c++)
		{
			float a = gRing[(i0 % RING_FRAMES) * 2 + c], b = gRing[((i0 + 1) % RING_FRAMES) * 2 + c];
			float v = (float)(a + (b - a) * frac) * gain * 32767.0f + samples[i * 2 + c];
			samples[i * 2 + c] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
		}
		gResamplePos += step;
	}
	{
		size_t whole = (size_t)gResamplePos;
		gRingRead += whole;
		gResamplePos -= (double)whole;
		if(gRingRead > gRingWrite)
			gRingRead = gRingWrite;
	}
	pthread_mutex_unlock(&gAudioLock);
}
