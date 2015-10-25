/*
    Copyright © 2014-2015 by The qTox Project

    This file is part of qTox, a Qt-based graphical interface for Tox.

    qTox is libre software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    qTox is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with qTox.  If not, see <http://www.gnu.org/licenses/>.
*/


// Output some extra debug info
#define AUDIO_DEBUG 1

// Fix a 7 years old openal-soft/alsa bug
// http://blog.gmane.org/gmane.comp.lib.openal.devel/month=20080501
// If set to 1, the capture will be started as long as the device is open
#define FIX_SND_PCM_PREPARE_BUG 0

#include "audio.h"
#include "src/core/core.h"
#include "src/persistence/settings.h"

#include <QDebug>
#include <QTimer>
#include <QThread>

#include <cassert>

Audio* Audio::instance{nullptr};

/**
Returns the singleton's instance. Will construct on first call.
*/
Audio& Audio::getInstance()
{
    if (!instance)
    {
        instance = new Audio();
        instance->startAudioThread();
    }
    return *instance;
}

Audio::Audio()
    : audioThread(new QThread())
    , audioInLock(QMutex::Recursive)
    , audioOutLock(QMutex::Recursive)
    , inputSubscriptions(0)
    , alOutDev(nullptr)
    , alInDev(nullptr)
    , outputVolume(1.0)
    , inputVolume(1.0)
    , alMainSource(0)
    , alContext(nullptr)
    , timer(new QTimer(this))
{
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &Audio::closeOutput);

    audioThread->setObjectName("qTox Audio");
    connect(audioThread, &QThread::finished, audioThread, &QThread::deleteLater);
}

Audio::~Audio()
{
    audioThread->exit();
    audioThread->wait();
    if (audioThread->isRunning())
        audioThread->terminate();
}

/**
Start the audio thread for capture and playback.
*/
void Audio::startAudioThread()
{
    if (!audioThread->isRunning())
        audioThread->start();
    else
        qWarning("Audio thread already started -> ignored.");

    moveToThread(audioThread);
}

/**
Returns the current output volume, between 0 and 1
*/
qreal Audio::getOutputVolume()
{
    QMutexLocker locker(&audioOutLock);
    return outputVolume;
}

/**
The volume must be between 0 and 1
*/
void Audio::setOutputVolume(qreal volume)
{
    QMutexLocker locker(&audioOutLock);
    outputVolume = volume;
    alSourcef(alMainSource, AL_GAIN, outputVolume);

    for (const ToxGroupCall& call : Core::groupCalls)
    {
        if (!call.active)
            continue;
        for (ALuint source : call.alSources)
            alSourcef(source, AL_GAIN, outputVolume);
    }

    for (const ToxCall& call : Core::calls)
    {
        if (!call.active)
            continue;
        alSourcef(call.alSource, AL_GAIN, outputVolume);
    }
}

/**
The volume must be between 0 and 2
*/
void Audio::setInputVolume(qreal volume)
{
    QMutexLocker locker(&audioInLock);
    inputVolume = volume;
}

/**
@brief Subscribe to capture sound from the opened input device.

If the input device is not open, it will be opened before capturing.
*/
void Audio::subscribeInput()
{
    qDebug() << "subscribing input" << inputSubscriptions;
    if (!inputSubscriptions++)
    {
        openInput(Settings::getInstance().getInDev());
        openOutput(Settings::getInstance().getOutDev());

#if (!FIX_SND_PCM_PREPARE_BUG)
        if (alInDev)
        {
            qDebug() << "starting capture";
            alcCaptureStart(alInDev);
        }
#endif
    }
}

/**
@brief Unsubscribe from capturing from an opened input device.

If the input device has no more subscriptions, it will be closed.
*/
void Audio::unsubscribeInput()
{
    qDebug() << "unsubscribing input" << inputSubscriptions;
    if (inputSubscriptions > 0)
        inputSubscriptions--;
    else if(inputSubscriptions < 0)
        inputSubscriptions = 0;

    if (!inputSubscriptions) {
        closeOutput();
        closeInput();
    }
}

/**
Open an input device, use before suscribing
*/
void Audio::openInput(const QString& inDevDescr)
{
    QMutexLocker lock(&audioInLock);

    if (alInDev) {
#if (!FIX_SND_PCM_PREPARE_BUG)
        qDebug() << "stopping capture";
        alcCaptureStop(alInDev);
#endif
        alcCaptureCloseDevice(alInDev);
    }
    alInDev = nullptr;

    int stereoFlag = av_DefaultSettings.audio_channels==1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
    const uint32_t sampleRate = av_DefaultSettings.audio_sample_rate;
    const uint16_t frameDuration = av_DefaultSettings.audio_frame_duration;
    const uint32_t chnls = av_DefaultSettings.audio_channels;
    const ALCsizei bufSize = (frameDuration * sampleRate * 4) / 1000 * chnls;
    if (inDevDescr.isEmpty())
        alInDev = alcCaptureOpenDevice(nullptr, sampleRate, stereoFlag, bufSize);
    else
        alInDev = alcCaptureOpenDevice(inDevDescr.toStdString().c_str(),
                                       sampleRate, stereoFlag, bufSize);

    if (alInDev)
        qDebug() << "Opening audio input "<<inDevDescr;
    else
        qWarning() << "Cannot open input audio device " + inDevDescr;

    Core* core = Core::getInstance();
    if (core)
        core->resetCallSources(); // Force to regen each group call's sources

    // Restart the capture if necessary
    if (alInDev)
    {
        alcCaptureStart(alInDev);
    }
    else
    {
#if (FIX_SND_PCM_PREPARE_BUG)
        alcCaptureStart(alInDev);
#endif
    }
}

/**
Open an output device
*/
bool Audio::openOutput(const QString &outDevDescr)
{
    qDebug() << "Opening audio output " + outDevDescr;
    QMutexLocker lock(&audioOutLock);

    auto* tmp = alOutDev;
    alOutDev = nullptr;
    if (outDevDescr.isEmpty())
        alOutDev = alcOpenDevice(nullptr);
    else
        alOutDev = alcOpenDevice(outDevDescr.toStdString().c_str());

    if (alOutDev)
    {
        if (alContext && alcMakeContextCurrent(nullptr) == ALC_TRUE)
            alcDestroyContext(alContext);

        if (tmp)
            alcCloseDevice(tmp);

        alContext = alcCreateContext(alOutDev, nullptr);
        if (alcMakeContextCurrent(alContext))
        {
            alGenSources(1, &alMainSource);
        }
        else
        {
            qWarning() << "Cannot create output audio context";
            alcCloseDevice(alOutDev);
            return false;
        }
    }
    else
    {
        qWarning() << "Cannot open output audio device " + outDevDescr;
        return false;
    }

    Core* core = Core::getInstance();
    if (core)
        core->resetCallSources(); // Force to regen each group call's sources

    return true;
}

/**
Close an input device, please don't use unless everyone's unsuscribed
*/
void Audio::closeInput()
{
    qDebug() << "Closing input";
    QMutexLocker locker(&audioInLock);
    if (alInDev)
    {
#if (!FIX_SND_PCM_PREPARE_BUG)
        qDebug() << "stopping capture";
        alcCaptureStop(alInDev);
#endif

        if (alcCaptureCloseDevice(alInDev) == ALC_TRUE)
        {
            alInDev = nullptr;
            inputSubscriptions = 0;
        }
        else
        {
            qWarning() << "Failed to close input";
        }
    }
}

/**
Close an output device
*/
void Audio::closeOutput()
{
    qDebug() << "Closing output";
    QMutexLocker locker(&audioOutLock);

    if (inputSubscriptions > 0)
        return;

    if (alContext && alcMakeContextCurrent(nullptr) == ALC_TRUE)
    {
        alcDestroyContext(alContext);
        alContext = nullptr;
    }

    if (alOutDev)
    {
        if (alcCloseDevice(alOutDev) == ALC_TRUE)
            alOutDev = nullptr;
        else
            qWarning() << "Failed to close output";
    }
}

/**
Play a 44100Hz mono 16bit PCM sound
*/
void Audio::playMono16Sound(const QByteArray& data)
{
    QMutexLocker lock(&audioOutLock);

    if (!alOutDev)
    {
        if (!openOutput(Settings::getInstance().getOutDev()))
            return;
    }

    ALuint buffer;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_MONO16, data.data(), data.size(), 44100);
    alSourcef(alMainSource, AL_GAIN, outputVolume);
    alSourcei(alMainSource, AL_BUFFER, buffer);
    alSourcePlay(alMainSource);

    ALint sizeInBytes;
    ALint channels;
    ALint bits;

    alGetBufferi(buffer, AL_SIZE, &sizeInBytes);
    alGetBufferi(buffer, AL_CHANNELS, &channels);
    alGetBufferi(buffer, AL_BITS, &bits);
    int lengthInSamples = sizeInBytes * 8 / (channels * bits);

    ALint frequency;
    alGetBufferi(buffer, AL_FREQUENCY, &frequency);
    qreal duration = (lengthInSamples / static_cast<qreal>(frequency)) * 1000;
    int remaining = timer->interval();

    if (duration > remaining)
        timer->start(duration);

    alDeleteBuffers(1, &buffer);
}

/**
@brief May be called from any thread, will always queue a call to playGroupAudio.

The first and last argument are ignored, but allow direct compatibility with toxcore.
*/
void Audio::playGroupAudioQueued(Tox*,int group, int peer, const int16_t* data,
                        unsigned samples, uint8_t channels, unsigned sample_rate, void* core)
{
    QMetaObject::invokeMethod(instance, "playGroupAudio", Qt::BlockingQueuedConnection,
                              Q_ARG(int,group), Q_ARG(int,peer), Q_ARG(const int16_t*,data),
                              Q_ARG(unsigned,samples), Q_ARG(uint8_t,channels), Q_ARG(unsigned,sample_rate));
    emit static_cast<Core*>(core)->groupPeerAudioPlaying(group, peer);
}



/**
Must be called from the audio thread, plays a group call's received audio
*/
void Audio::playGroupAudio(int group, int peer, const int16_t* data,
                           unsigned samples, uint8_t channels, unsigned sample_rate)
{
    assert(QThread::currentThread() == audioThread);
    QMutexLocker lock(&audioOutLock);

    ToxGroupCall& call = Core::groupCalls[group];

    if (!call.active || call.muteVol)
        return;

    if (!call.alSources.contains(peer))
    {
        alGenSources(1, &call.alSources[peer]);
        alSourcef(call.alSources[peer], AL_GAIN, outputVolume);
    }

    qreal volume = 0.;
    int bufsize = samples * 2 * channels;
    for (int i = 0; i < bufsize; ++i)
        volume += abs(data[i]);

    emit groupAudioPlayed(group, peer, volume / bufsize);

    playAudioBuffer(call.alSources[peer], data, samples, channels, sample_rate);
}

void Audio::playAudioBuffer(ALuint alSource, const int16_t *data, int samples, unsigned channels, int sampleRate)
{
    assert(channels == 1 || channels == 2);

    QMutexLocker lock(&audioOutLock);

    ALuint bufid;
    ALint processed = 0, queued = 16;
    alGetSourcei(alSource, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(alSource, AL_BUFFERS_QUEUED, &queued);
    alSourcei(alSource, AL_LOOPING, AL_FALSE);

    if (processed)
    {
        ALuint bufids[processed];
        alSourceUnqueueBuffers(alSource, processed, bufids);
        alDeleteBuffers(processed - 1, bufids + 1);
        bufid = bufids[0];
    }
    else if (queued < 16)
    {
        alGenBuffers(1, &bufid);
    }
    else
    {
        qDebug() << "Dropped frame";
        return;
    }

    alBufferData(bufid, (channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, data,
                    samples * 2 * channels, sampleRate);
    alSourceQueueBuffers(alSource, 1, &bufid);

    ALint state;
    alGetSourcei(alSource, AL_SOURCE_STATE, &state);
    alSourcef(alSource, AL_GAIN, outputVolume);
    if (state != AL_PLAYING)
        alSourcePlay(alSource);
}

/**
Returns true if the input device is open and suscribed to
*/
bool Audio::isInputReady()
{
    QMutexLocker locker(&audioInLock);
    return alInDev && inputSubscriptions;
}

/**
Returns true if the output device is open
*/
bool Audio::isOutputClosed()
{
    QMutexLocker locker(&audioOutLock);
    return alOutDev;
}

/**
Does nothing and return false on failure
*/
bool Audio::tryCaptureSamples(uint8_t* buf, int framesize)
{
    QMutexLocker lock(&audioInLock);

    ALint samples=0;
    alcGetIntegerv(Audio::alInDev, ALC_CAPTURE_SAMPLES, sizeof(samples), &samples);
    if (samples < framesize)
        return false;

    memset(buf, 0, framesize * 2 * av_DefaultSettings.audio_channels); // Avoid uninitialized values (Valgrind)
    alcCaptureSamples(Audio::alInDev, buf, framesize);

    if (inputVolume != 1)
    {
        int16_t* bufReal = reinterpret_cast<int16_t*>(buf);
        for (int i = 0; i < framesize; ++i)
        {
            int sample = bufReal[i] * pow(inputVolume, 2);

            if (sample < std::numeric_limits<int16_t>::min())
                sample = std::numeric_limits<int16_t>::min();
            else if (sample > std::numeric_limits<int16_t>::max())
                sample = std::numeric_limits<int16_t>::max();

            bufReal[i] = sample;
        }
    }

    return true;
}

#ifdef QTOX_FILTER_AUDIO
#include "audiofilterer.h"

/* include for compatibility with older versions of OpenAL */
#ifndef ALC_ALL_DEVICES_SPECIFIER
#include <AL/alext.h>
#endif

void Audio::getEchoesToFilter(AudioFilterer* filterer, int framesize)
{
#ifdef ALC_LOOPBACK_CAPTURE_SAMPLES
    ALint samples;
    alcGetIntegerv(Audio::alOutDev, ALC_LOOPBACK_CAPTURE_SAMPLES, sizeof(samples), &samples);
    if (samples >= framesize)
    {
        int16_t buf[framesize];
        alcCaptureSamplesLoopback(Audio::alOutDev, buf, framesize);
        filterer->passAudioOutput(buf, framesize);
        filterer->setEchoDelayMs(5); // This 5ms is configurable I believe
    }
#else
    Q_UNUSED(filterer);
    Q_UNUSED(framesize);
#endif
}
#endif
