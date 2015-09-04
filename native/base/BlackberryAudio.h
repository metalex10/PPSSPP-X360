/*
 * Copyright (c) 2013 Sacha Refshauge
 *
 */
// Blackberry implementation of the framework.
#ifndef BLACKBERRYAUDIO_H
#define BLACKBERRYAUDIO_H

#include <AL/al.h>
#include <AL/alc.h>

#include "base/NativeApp.h"

#define AUDIO_FREQ 44100
#define SAMPLE_SIZE 2048
class BlackberryAudio
{
public:
	BlackberryAudio()
	{
		alcDevice = alcOpenDevice(NULL);
		if (alContext = alcCreateContext(alcDevice, NULL))
			alcMakeContextCurrent(alContext);
		alGenSources(1, &source);
		alGenBuffers(1, &buffer);
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&thread_handle, &attr, &BlackberryAudio::staticThreadProc, this);
	}
	~BlackberryAudio()
	{
		pthread_cancel(thread_handle);
		alcMakeContextCurrent(NULL);
		if (alContext)
		{
			alcDestroyContext(alContext);
			alContext = NULL;
		}
		if (alcDevice)
		{
			alcCloseDevice(alcDevice);
			alcDevice = NULL;
		}
	}
	static void* staticThreadProc(void* arg)
	{
		return reinterpret_cast<BlackberryAudio*>(arg)->RunAudio();
	}
private:
	void* RunAudio()
	{
		while(true)
		{
			size_t frames_ready;
			alGetSourcei(source, AL_SOURCE_STATE, &state);
			if (state != AL_PLAYING) {
				frames_ready = NativeMix((short*)stream, 5*SAMPLE_SIZE);
			}
			else
				frames_ready = 0;
			if (frames_ready > 0)
			{
				const size_t bytes_ready = frames_ready * sizeof(short) * 2;
				alSourcei(source, AL_BUFFER, 0);
				alBufferData(buffer, AL_FORMAT_STEREO16, stream, bytes_ready, AUDIO_FREQ);
				alSourcei(source, AL_BUFFER, buffer);
				alSourcePlay(source);
				// TODO: Maybe this could get behind?
				usleep((1000000 * SAMPLE_SIZE) / AUDIO_FREQ);
			}
			else
				usleep(10000);
		}
	}
	ALCdevice *alcDevice;
	ALCcontext *alContext;
	ALenum state;
	ALuint buffer;
	ALuint source;
	char stream[20*SAMPLE_SIZE];
	pthread_t thread_handle;
};
#endif

