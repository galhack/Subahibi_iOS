/*
 * This file is part of OpenBGI (https://openbgi.net).
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * audio.c - the Apple back end's sound output (inc/bgi/os.h, "Audio")
 *           through an AudioToolbox output unit
 *
 * The engine's mixer is pulled from the unit's render callback in 16-bit
 * stereo at the engine's rate; the unit converts to the hardware format.
 * The movie player's sound (movie.c) is mixed in after the engine's.  The
 * application configures the audio session (category, activation) before
 * the engine starts.  CD audio and PlaySound have no counterpart here.
 */
#include "apple_internal.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

static AudioComponentInstance gUnit;
static OsAudioFill_t gFill;
static void* gUser;
static int gRate;

static OSStatus Render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
	UInt32 frames, AudioBufferList* io)
{
	int16_t* out = (int16_t*)io->mBuffers[0].mData;
	BGI_UNUSED(ref);
	BGI_UNUSED(flags);
	BGI_UNUSED(ts);
	BGI_UNUSED(bus);
	if(gFill)
		gFill(gUser, out, (int)frames);
	else
		memset(out, 0, (size_t)frames * 4);
	AppleMovie_MixAudio(out, (int)frames, gRate);
	return noErr;
}

int OS_AudioStart(int rate, OsAudioFill_t fill, void* user)
{
	AudioComponentDescription desc;
	AudioComponent comp;
	AudioStreamBasicDescription fmt;
	AURenderCallbackStruct cb;
	if(gUnit)
		OS_AudioStop();
	memset(&desc, 0, sizeof desc);
	desc.componentType = kAudioUnitType_Output;
#if TARGET_OS_OSX
	desc.componentSubType = kAudioUnitSubType_DefaultOutput;
#else
	desc.componentSubType = kAudioUnitSubType_RemoteIO;
#endif
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	comp = AudioComponentFindNext(NULL, &desc);
	if(!comp || AudioComponentInstanceNew(comp, &gUnit) != noErr)
	{
		gUnit = NULL;
		return 0;
	}
	memset(&fmt, 0, sizeof fmt);
	fmt.mSampleRate = rate;
	fmt.mFormatID = kAudioFormatLinearPCM;
	fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
	fmt.mChannelsPerFrame = 2;
	fmt.mBitsPerChannel = 16;
	fmt.mBytesPerFrame = 4;
	fmt.mFramesPerPacket = 1;
	fmt.mBytesPerPacket = 4;
	gFill = fill;
	gUser = user;
	gRate = rate;
	cb.inputProc = Render;
	cb.inputProcRefCon = NULL;
	if(AudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt) != noErr ||
		AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
		AudioUnitInitialize(gUnit) != noErr || AudioOutputUnitStart(gUnit) != noErr)
	{
		AudioComponentInstanceDispose(gUnit);
		gUnit = NULL;
		gFill = NULL;
		gRate = 0;
		return 0;
	}
	return 1;
}

// stop and dispose of the unit; disposing waits for a render callback in progress
void OS_AudioStop(void)
{
	if(!gUnit)
		return;
	AudioOutputUnitStop(gUnit);
	AudioUnitUninitialize(gUnit);
	AudioComponentInstanceDispose(gUnit);
	gUnit = NULL;
	gFill = NULL;
	gRate = 0;
}

int OS_AudioAvailable(void)
{
	return 1;
}

int AppleAudio_Rate(void)
{
	return gRate;
}

// ---- CD audio and PlaySound: none ----------------------------------------------------------

uint32_t OS_MciOpenCdAudio(const char* element)
{
	BGI_UNUSED(element);
	return 0;
}

int OS_MciSetTimeFormatTmsf(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciClose(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_MciStatus(uint32_t dev, uint32_t item, uint32_t* out)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(item);
	*out = 0;
	return 0;
}

int OS_MciPlay(uint32_t dev, uint32_t from, uint32_t to, int notify)
{
	BGI_UNUSED(dev);
	BGI_UNUSED(from);
	BGI_UNUSED(to);
	BGI_UNUSED(notify);
	return 0;
}

int OS_MciStop(uint32_t dev)
{
	BGI_UNUSED(dev);
	return 0;
}

int OS_PlaySoundFile(const char* path)
{
	BGI_UNUSED(path);
	return 0;
}
