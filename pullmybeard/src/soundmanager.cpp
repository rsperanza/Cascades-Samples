/* Copyright (c) 2012 Research In Motion Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <QDir>
#include <qdebug.h>
#include <sys/types.h>

#include "soundmanager.h"

typedef struct
{
	char    tag[4];
	long    length;
}
riff_tag;

typedef struct
{
	riff_tag formatTag;
	short   format_tag;
	short   channels;
	long    samples_per_sec;
	long    avg_bytes_per_sec;
	short   block_align;
	short   bits_per_sample;
}
wav_format;

typedef struct
{
	riff_tag riffTag;
	char    Wave[4];
	wav_format wavFormat;
	riff_tag wavData;
}
wave_hdr;


const ALCchar* captureDeviceName = NULL;
const ALCuint  captureFrequency  = 44100;
//const ALCenum  captureFormat     = AL_FORMAT_MONO16;
const ALCenum  captureFormat     = AL_FORMAT_STEREO16;
#define FMTSIZE 4 // 4 bytes per stereo sample
const ALCsizei captureBufferSamples = (captureFrequency / 4);
const ALCsizei captureBufferSize = (captureBufferSamples * FMTSIZE);


// Error message function for ALUT.
static void reportALUTError()
{
    qDebug() << "ALUT reported the following error: " << alutGetErrorString(alutGetError());
}

// Error message function for OpenAL.
static void reportOpenALError()
{
    qDebug() << "OpenAL reported the following error: \n" << alutGetErrorString(alGetError());
}


static void writeWaveHeader(QFile &file, int sampleRate, int nChannels)
{
	wave_hdr waveHeader;

    waveHeader.riffTag = { {'R', 'I', 'F', 'F'}, 0 };
    waveHeader.Wave[0] = 'W';
    waveHeader.Wave[1] = 'A';
    waveHeader.Wave[2] = 'V';
    waveHeader.Wave[3] = 'E';
    waveHeader.wavFormat.formatTag = { { 'f', 'm', 't', ' ' }, (sizeof(waveHeader.wavFormat)-8) };

    waveHeader.wavFormat.format_tag = 1;
    waveHeader.wavFormat.channels = nChannels;
    waveHeader.wavFormat.samples_per_sec = sampleRate;
    waveHeader.wavFormat.avg_bytes_per_sec = nChannels * 2;
    waveHeader.wavFormat.block_align = nChannels * 2;
    waveHeader.wavFormat.bits_per_sample = 16;

    waveHeader.wavData = { {'d', 'a', 't', 'a'}, 0 };

	file.write((const char *)&waveHeader, sizeof(waveHeader));
	file.flush();
}

static void updateWaveHeader(QFile &file, int dataSize)
{
	wave_hdr waveHeader;

   file.seek(4); // seek to RIFF size
   int size = dataSize + sizeof(waveHeader) - 8;
   file.write((const char *)&size, 4);
   file.flush();

   file.seek(sizeof(waveHeader) - 4); // seek to DATA size
   file.write((const char *)&dataSize, 4);
   file.flush();
}

static void* saveCapture(void *dummy)
{
    SoundManager* soundManager = (SoundManager*)dummy;

    ALenum _AL_FORMAT_MONO_FLOAT32 = 0;
    ALenum _AL_FORMAT_STEREO_FLOAT32 = 0;
    ALboolean ext;
/*
    // Initialize the ALUT.
    if (alutInit(NULL, NULL) == false) {
        reportALUTError();
    }
*/
    if (!alcIsExtensionPresent(NULL, "ALC_EXT_capture"))
    {
    	qDebug() << "No ALC_EXT_capture support.\n";
        exit(42);
    } // if

    // these may not exist, depending on the implementation.
    _AL_FORMAT_MONO_FLOAT32 = alGetEnumValue("AL_FORMAT_MONO_FLOAT32");
    _AL_FORMAT_STEREO_FLOAT32 = alGetEnumValue("AL_FORMAT_STEREO_FLOAT32");
    alGetError();

    #define printALCString(dev, ext) { ALenum e = alcGetEnumValue(dev, #ext); fprintf(stderr, "%s: %s\n", #ext, alcGetString(dev, e)); }
    printALCString(NULL, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);

    ext = alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT");
    if (!ext)
        fprintf(stderr, "No ALC_ENUMERATION_EXT support.\n");
    else
    {
        char *devList;
        ALenum alenum = alcGetEnumValue(NULL, "ALC_CAPTURE_DEVICE_SPECIFIER");
		devList = (char *)alcGetString(NULL, alenum);

        fprintf(stderr, "ALC_ENUMERATION_EXT:\n");
        while (*devList)  // I really hate this double null terminated list thing.
        {
            fprintf(stderr, "  - %s\n", devList);
            devList += strlen(devList) + 1;
        } // while
    } // else

    // Main capture loop.

    bool capturing = true;
    int numSamples = 0;
    int dataSize = 0;

    QFileInfo fileInfo(soundManager->_captureFilename);
    QFile file(soundManager->_captureFilename);

    // Open the file that was created
    if (!file.exists()) {
        QDir().mkpath (fileInfo.dir().path());
    }
    if (file.open(QIODevice::WriteOnly)) {

    	writeWaveHeader(file, captureFrequency, 2);

        soundManager->_saveBuffer = new ALbyte[captureBufferSize*FMTSIZE];
        soundManager->_captureDevice = alcCaptureOpenDevice( captureDeviceName, captureFrequency, captureFormat, captureBufferSize*FMTSIZE );
        if (soundManager->_captureDevice == NULL) {
        	qCritical() << "Couldn't open capture device.\n";
        	if (alGetError() != AL_NO_ERROR) {
        		reportOpenALError();
        	}
        }

        if (soundManager->_captureDevice)
        {
			// start capture
			alcCaptureStart( soundManager->_captureDevice );
			if (alGetError() != AL_NO_ERROR) {
				reportOpenALError();
			}

			while( capturing ) {
				pthread_mutex_lock( &soundManager->_captureMutex );
				capturing = soundManager->_capturing;
				pthread_mutex_unlock( &soundManager->_captureMutex );

				qDebug() << "capturing: " << capturing;

				if (capturing) {
					alcGetIntegerv( soundManager->_captureDevice, ALC_CAPTURE_SAMPLES, 4, &numSamples );
					if (numSamples >= captureBufferSamples) {
						alcCaptureSamples( soundManager->_captureDevice, soundManager->_saveBuffer, captureBufferSamples );
						qDebug() << "saving samples: " << numSamples;
						file.write((const char *)soundManager->_saveBuffer, captureBufferSize);
						dataSize += captureBufferSize*FMTSIZE;
						file.flush();
					}
					qDebug() << "samples: " << numSamples;

					usleep(50);
				}
			}

			// get what samples are left over
			alcGetIntegerv( soundManager->_captureDevice, ALC_CAPTURE_SAMPLES, 4, &numSamples );
			alcCaptureSamples( soundManager->_captureDevice, soundManager->_saveBuffer, numSamples );
			dataSize += numSamples*FMTSIZE;

			qDebug() << "saving samples: " << numSamples;

			file.write((const char *)soundManager->_saveBuffer, numSamples*FMTSIZE);
			file.flush();

			qDebug() << "samples: " << numSamples;


			qDebug() << "updating wave header ... ";
			updateWaveHeader(file, dataSize);


			// start capture
			qDebug() << "stopping capture ... ";
			alcCaptureStop( soundManager->_captureDevice );
			if (alGetError() != AL_NO_ERROR) {
				reportOpenALError();
			}

			qDebug() << "closing capture device ... ";
			alcCaptureCloseDevice(soundManager->_captureDevice);
			if (alGetError() != AL_NO_ERROR) {
				reportOpenALError();
			}
        } // if

		file.close();

		delete soundManager->_saveBuffer;
	}
/*
	// Exit the ALUT.
	if (alutExit() == false) {
		reportALUTError();
	}
*/

    return NULL;
}

/*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alut.h"
#include <unistd.h>

ALenum _AL_FORMAT_MONO_FLOAT32 = 0;
ALenum _AL_FORMAT_STEREO_FLOAT32 = 0;

#define FMT AL_FORMAT_MONO16;
#define FMTSIZE 8
#define FREQ 44100
#define SAMPS (FREQ * 5)
static ALbyte buf[SAMPS*FMTSIZE];
static ALCdevice *in = NULL;

ALvoid *recordSomething(void)
{
    ALint samples = 0;
    ALenum capture_samples = alcGetEnumValue(in, "ALC_CAPTURE_SAMPLES");
    fprintf(stderr, "recording...\n");
    alcCaptureStart(in);

    while (samples < SAMPS)
    {
        alcGetIntegerv(in, capture_samples, sizeof (samples), &samples);
    } // while

    alcCaptureSamples(in, buf, SAMPS);
    alcCaptureStop(in);
    return(buf);  // buf has SAMPS samples of FMT audio at FREQ.
} // recordSomething


int main(int argc, char **argv)
{
    ALboolean ext;
    ALuint sid;
    ALuint bid;
    ALint state = AL_PLAYING;

    if (!alcIsExtensionPresent(NULL, "ALC_EXT_capture"))
    {
        fprintf(stderr, "No ALC_EXT_capture support.\n");
        return(42);
    } // if

    // these may not exist, depending on the implementation.
    _AL_FORMAT_MONO_FLOAT32 = alGetEnumValue("AL_FORMAT_MONO_FLOAT32");
    _AL_FORMAT_STEREO_FLOAT32 = alGetEnumValue("AL_FORMAT_STEREO_FLOAT32");
    alGetError();

    #define printALCString(dev, ext) { ALenum e = alcGetEnumValue(dev, #ext); fprintf(stderr, "%s: %s\n", #ext, alcGetString(dev, e)); }
    printALCString(NULL, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);

    ext = alcIsExtensionPresent(NULL, "ALC_ENUMERATION_EXT");
    if (!ext)
        fprintf(stderr, "No ALC_ENUMERATION_EXT support.\n");
    else
    {
        char *devList;
        ALenum alenum = alcGetEnumValue(NULL, "ALC_CAPTURE_DEVICE_SPECIFIER");
		devList = (char *)alcGetString(NULL, alenum);

        fprintf(stderr, "ALC_ENUMERATION_EXT:\n");
        while (*devList)  // I really hate this double null terminated list thing.
        {
            fprintf(stderr, "  - %s\n", devList);
            devList += strlen(devList) + 1;
        } // while
    } // else

    alutInit(&argc, argv);
    in = alcCaptureOpenDevice(0, FREQ, AL_FORMAT_MONO16, SAMPS);

    if (in == NULL)
    {
        fprintf(stderr, "Couldn't open capture device.\n");
        alutExit();
        return(42);
    } // if

    recordSomething();

    alcCaptureCloseDevice(in);

    alGenSources(1, &sid);
    alGenBuffers(1, &bid);

    fprintf(stderr, "Playing...\n");

    alBufferData(bid, AL_FORMAT_MONO16, buf, sizeof(buf), FREQ);
    alSourcei(sid, AL_BUFFER, bid);
    alSourcePlay(sid);

    while (state == AL_PLAYING)
    {
        alGetSourcei(sid, AL_SOURCE_STATE, &state);
    } // while

    fprintf(stderr, "Cleaning up...\n");

    alDeleteSources(1, &sid);
    alDeleteBuffers(1, &bid);

    alutExit();
    return(0);
} // main

 */

SoundManager::SoundManager(QString soundDirectory)
{
    QString applicationDirectory;
    QString completeSoundDirectory;
    char cwd[PATH_MAX];
    ALuint bufferID;

    // Initialize the ALUT.
    if (alutInit(NULL, NULL) == false) {
        reportALUTError();
    }

    // Get the complete application directory in which we will load sounds from
    // Convert to QString since it is more convenient when working with directories.
    getcwd(cwd, PATH_MAX);
    applicationDirectory = QString(cwd);

    // Append the assets directory and the actual sounds directory name to the QString.
    completeSoundDirectory = applicationDirectory.append("/app/native/assets/").append(
            soundDirectory);

    // Create OpenAL buffers from all files in the sound directory.
    QDir dir(completeSoundDirectory);

    if (!dir.exists()) {
        qDebug() << "Cannot find the sounds directory." << completeSoundDirectory;
    } else {

        // Set a filter for file listing (only files should be listed).
        dir.setFilter(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot);

        // Get a directory listing.
        QFileInfoList list = dir.entryInfoList();

        // Traverse all the files in the directory.
        for (int i = 0; i < list.size(); ++i) {

            // Attempt to create buffers for all the files.
            QFileInfo fileInfo = list.at(i);
            bufferID = alutCreateBufferFromFile(fileInfo.absoluteFilePath().toStdString().c_str());

            if (alutGetError() == ALUT_ERROR_NO_ERROR) {

                // Add the bufferID to the Hash table with the file name as key.
                mSoundBuffers[fileInfo.fileName()] = bufferID;

            } else {
                reportALUTError();
            }
        }
    }

    // Generate a number of sources used to attach buffers and play the sounds.
    alGenSources(SOUNDMANAGER_MAX_NBR_OF_SOURCES, mSoundSources);

    if (alGetError() != AL_NO_ERROR) {
        reportOpenALError();
    }

    if ( pthread_mutex_init( &_captureMutex, NULL ) != 0 ) {
        qCritical() << "SoundManager: capture mutex not created!";
    }
}

SoundManager::~SoundManager()
{
    ALuint bufferID = 0;

    // Clear all the sources.
    for (int sourceIndex = 0; sourceIndex < SOUNDMANAGER_MAX_NBR_OF_SOURCES; sourceIndex++) {
        ALuint source = mSoundSources[sourceIndex];
        alDeleteSources(1, &source);

        if (alGetError() != AL_NO_ERROR) {
            reportOpenALError();
        }
    }

    // Clear buffers then iterate through the hash table.
    QHashIterator<QString, ALuint> iterator(mSoundBuffers);

    while (iterator.hasNext()) {
        iterator.next();

        // Get the buffer id and delete it.
        bufferID = mSoundBuffers[iterator.key()];
        if (bufferID != 0) {
            alDeleteBuffers(1, &bufferID);

            if (alGetError() != AL_NO_ERROR) {
                reportOpenALError();
            }
        }
    }

    // Clear the QHash for sound buffer ids.
    mSoundBuffers.clear();

	pthread_mutex_destroy( &_captureMutex );

    // Exit the ALUT.
    if (alutExit() == false) {
        reportALUTError();
    }
}

bool SoundManager::play(QString fileName, float pitch, float gain)
{
    static uint sourceIndex = 0;

    // Get the corresponding buffer id set up in the init function.
    ALuint bufferID = mSoundBuffers[fileName];

    if (bufferID != 0) {
        // Increment which source we are using, so that we play in a "free" source.
        sourceIndex = (sourceIndex + 1) % SOUNDMANAGER_MAX_NBR_OF_SOURCES;

        // Get the source in which the sound will be played.
        ALuint source = mSoundSources[sourceIndex];

        if (alIsSource(source) == AL_TRUE) {

            // Attach the buffer to an available source.
            alSourcei(source, AL_BUFFER, bufferID);

            if (alGetError() != AL_NO_ERROR) {
                reportOpenALError();
                return false;
            }

            // Set the source pitch value.
            alSourcef(source, AL_PITCH, pitch);

            if (alGetError() != AL_NO_ERROR) {
                reportOpenALError();
                return false;
            }

            // Set the source gain value.
            alSourcef(source, AL_GAIN, gain);

            if (alGetError() != AL_NO_ERROR) {
                reportOpenALError();
                return false;
            }

            // Play the source.
            alSourcePlay(source);

            if (alGetError() != AL_NO_ERROR) {
                reportOpenALError();
                return false;
            }
        }
    } else {
        // The buffer was not found, so return false.
        return false;
    }

    return true;
}

bool SoundManager::play(QString fileName)
{
    // Play the sound with default gain and pitch values.
    return play(fileName, 1.0f, 1.0f);
}


bool SoundManager::startCapture(QString fileName)
{
    _captureFilename = fileName;

	pthread_mutex_lock( &_captureMutex );
	_capturing = true;
	pthread_mutex_unlock( &_captureMutex );



    int policy;
    struct sched_param param;
    int error;
    pthread_attr_t attr;


    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setinheritsched (&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_getschedparam (pthread_self (), &policy, &param);
    param.sched_priority=12;
    pthread_attr_setschedparam (&attr, &param);
    pthread_attr_setschedpolicy (&attr, SCHED_RR);

    if( (error = pthread_create( &_captureThread, &attr, saveCapture, this ) ) != 0 ) {
        qCritical() << "Unable to create tone thread " << strerror(error) << "\n";
        //snd_pcm_close (playback_handle);
        //free( frag_buffer );
        //pthread_mutex_destroy( &toneMutex );
        //pthread_cond_destroy( &condvar_newtone );

        //live = STOPPED;
        //if( audioman_handle ) {
        //   audio_manager_free_handle( audioman_handle );
        //}
        //pthread_attr_destroy(&attr);
        //return error;
    }

    pthread_attr_destroy(&attr);

	return true;
}

bool SoundManager::stopCapture()
{
	pthread_mutex_lock( &_captureMutex );

	_capturing = false;

	pthread_mutex_unlock( &_captureMutex );

	pthread_join( _captureThread, NULL );

	return true;
}

