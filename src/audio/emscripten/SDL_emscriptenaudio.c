/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"

#if SDL_AUDIO_DRIVER_EMSCRIPTEN

#include "../SDL_audio_c.h"
#include "SDL_emscriptenaudio.h"

#include <emscripten/emscripten.h>

/* !!! FIXME: this currently expects that the audio callback runs in the main thread,
   !!! FIXME:  in intervals when the application isn't running, but that may not be
   !!! FIXME:  true always once pthread support becomes widespread. Revisit this code
   !!! FIXME:  at some point and see what needs to be done for that! */

static void
FeedAudioDevice(_THIS, const void *buf, const int buflen)
{
    const int framelen = (SDL_AUDIO_BITSIZE(this->spec.format) / 8) * this->spec.channels;
    MAIN_THREAD_EM_ASM({
        var SDL3 = Module['SDL3'];
        var numChannels = SDL3.audio.currentOutputBuffer['numberOfChannels'];
        for (var c = 0; c < numChannels; ++c) {
            var channelData = SDL3.audio.currentOutputBuffer['getChannelData'](c);
            if (channelData.length != $1) {
                throw 'Web Audio output buffer length mismatch! Destination size: ' + channelData.length + ' samples vs expected ' + $1 + ' samples!';
            }

            for (var j = 0; j < $1; ++j) {
                channelData[j] = HEAPF32[$0 + ((j*numChannels + c) << 2) >> 2];  /* !!! FIXME: why are these shifts here? */
            }
        }
    }, buf, buflen / framelen);
}

static void
HandleAudioProcess(_THIS)
{
    SDL_AudioCallback callback = this->callbackspec.callback;
    const int stream_len = this->callbackspec.size;

    /* Only do something if audio is enabled */
    if (!SDL_AtomicGet(&this->enabled) || SDL_AtomicGet(&this->paused)) {
        if (this->stream) {
            SDL_AudioStreamClear(this->stream);
        }

        SDL_memset(this->work_buffer, this->spec.silence, this->spec.size);
        FeedAudioDevice(this, this->work_buffer, this->spec.size);
        return;
    }

    if (this->stream == NULL) {  /* no conversion necessary. */
        SDL_assert(this->spec.size == stream_len);
        callback(this->callbackspec.userdata, this->work_buffer, stream_len);
    } else {  /* streaming/converting */
        int got;
        while (SDL_AudioStreamAvailable(this->stream) < ((int) this->spec.size)) {
            callback(this->callbackspec.userdata, this->work_buffer, stream_len);
            if (SDL_AudioStreamPut(this->stream, this->work_buffer, stream_len) == -1) {
                SDL_AudioStreamClear(this->stream);
                SDL_AtomicSet(&this->enabled, 0);
                break;
            }
        }

        got = SDL_AudioStreamGet(this->stream, this->work_buffer, this->spec.size);
        SDL_assert((got < 0) || (got == this->spec.size));
        if (got != this->spec.size) {
            SDL_memset(this->work_buffer, this->spec.silence, this->spec.size);
        }
    }

    FeedAudioDevice(this, this->work_buffer, this->spec.size);
}

static void
HandleCaptureProcess(_THIS)
{
    SDL_AudioCallback callback = this->callbackspec.callback;
    const int stream_len = this->callbackspec.size;

    /* Only do something if audio is enabled */
    if (!SDL_AtomicGet(&this->enabled) || SDL_AtomicGet(&this->paused)) {
        SDL_AudioStreamClear(this->stream);
        return;
    }

    MAIN_THREAD_EM_ASM({
        var SDL3 = Module['SDL3'];
        var numChannels = SDL3.capture.currentCaptureBuffer.numberOfChannels;
        for (var c = 0; c < numChannels; ++c) {
            var channelData = SDL3.capture.currentCaptureBuffer.getChannelData(c);
            if (channelData.length != $1) {
                throw 'Web Audio capture buffer length mismatch! Destination size: ' + channelData.length + ' samples vs expected ' + $1 + ' samples!';
            }

            if (numChannels == 1) {  /* fastpath this a little for the common (mono) case. */
                for (var j = 0; j < $1; ++j) {
                    setValue($0 + (j * 4), channelData[j], 'float');
                }
            } else {
                for (var j = 0; j < $1; ++j) {
                    setValue($0 + (((j * numChannels) + c) * 4), channelData[j], 'float');
                }
            }
        }
    }, this->work_buffer, (this->spec.size / sizeof (float)) / this->spec.channels);

    /* okay, we've got an interleaved float32 array in C now. */

    if (this->stream == NULL) {  /* no conversion necessary. */
        SDL_assert(this->spec.size == stream_len);
        callback(this->callbackspec.userdata, this->work_buffer, stream_len);
    } else {  /* streaming/converting */
        if (SDL_AudioStreamPut(this->stream, this->work_buffer, this->spec.size) == -1) {
            SDL_AtomicSet(&this->enabled, 0);
        }

        while (SDL_AudioStreamAvailable(this->stream) >= stream_len) {
            const int got = SDL_AudioStreamGet(this->stream, this->work_buffer, stream_len);
            SDL_assert((got < 0) || (got == stream_len));
            if (got != stream_len) {
                SDL_memset(this->work_buffer, this->callbackspec.silence, stream_len);
            }
            callback(this->callbackspec.userdata, this->work_buffer, stream_len);  /* Send it to the app. */
        }
    }
}


static void
EMSCRIPTENAUDIO_CloseDevice(_THIS)
{
    MAIN_THREAD_EM_ASM({
        var SDL3 = Module['SDL3'];
        if ($0) {
            if (SDL3.capture.silenceTimer !== undefined) {
                clearTimeout(SDL3.capture.silenceTimer);
            }
            if (SDL3.capture.stream !== undefined) {
                var tracks = SDL3.capture.stream.getAudioTracks();
                for (var i = 0; i < tracks.length; i++) {
                    SDL3.capture.stream.removeTrack(tracks[i]);
                }
                SDL3.capture.stream = undefined;
            }
            if (SDL3.capture.scriptProcessorNode !== undefined) {
                SDL3.capture.scriptProcessorNode.onaudioprocess = function(audioProcessingEvent) {};
                SDL3.capture.scriptProcessorNode.disconnect();
                SDL3.capture.scriptProcessorNode = undefined;
            }
            if (SDL3.capture.mediaStreamNode !== undefined) {
                SDL3.capture.mediaStreamNode.disconnect();
                SDL3.capture.mediaStreamNode = undefined;
            }
            if (SDL3.capture.silenceBuffer !== undefined) {
                SDL3.capture.silenceBuffer = undefined
            }
            SDL3.capture = undefined;
        } else {
            if (SDL3.audio.scriptProcessorNode != undefined) {
                SDL3.audio.scriptProcessorNode.disconnect();
                SDL3.audio.scriptProcessorNode = undefined;
            }
            SDL3.audio = undefined;
        }
        if ((SDL3.audioContext !== undefined) && (SDL3.audio === undefined) && (SDL3.capture === undefined)) {
            SDL3.audioContext.close();
            SDL3.audioContext = undefined;
        }
    }, this->iscapture);

#if 0  /* !!! FIXME: currently not used. Can we move some stuff off the SDL3 namespace? --ryan. */
    SDL_free(this->hidden);
#endif
}

static int
EMSCRIPTENAUDIO_OpenDevice(_THIS, const char *devname)
{
    SDL_AudioFormat test_format;
    SDL_bool iscapture = this->iscapture;
    int result;

    /* based on parts of library_sdl.js */

    /* create context */
    result = MAIN_THREAD_EM_ASM_INT({
        if (typeof(Module['SDL3']) === 'undefined') {
            Module['SDL3'] = {};
        }
        var SDL3 = Module['SDL3'];
        if (!$0) {
            SDL3.audio = {};
        } else {
            SDL3.capture = {};
        }

        if (!SDL3.audioContext) {
            if (typeof(AudioContext) !== 'undefined') {
                SDL3.audioContext = new AudioContext();
            } else if (typeof(webkitAudioContext) !== 'undefined') {
                SDL3.audioContext = new webkitAudioContext();
            }
            if (SDL3.audioContext) {
                autoResumeAudioContext(SDL3.audioContext);
            }
        }
        return SDL3.audioContext === undefined ? -1 : 0;
    }, iscapture);
    if (result < 0) {
        return SDL_SetError("Web Audio API is not available!");
    }

    for (test_format = SDL_FirstAudioFormat(this->spec.format); test_format; test_format = SDL_NextAudioFormat()) {
        switch (test_format) {
        case AUDIO_F32: /* web audio only supports floats */
            break;
        default:
            continue;
        }
        break;
    }

    if (!test_format) {
        /* Didn't find a compatible format :( */
        return SDL_SetError("%s: Unsupported audio format", "emscripten");
    }
    this->spec.format = test_format;

    /* Initialize all variables that we clean on shutdown */
#if 0  /* !!! FIXME: currently not used. Can we move some stuff off the SDL3 namespace? --ryan. */
    this->hidden = (struct SDL_PrivateAudioData *)
        SDL_malloc((sizeof *this->hidden));
    if (this->hidden == NULL) {
        return SDL_OutOfMemory();
    }
    SDL_zerop(this->hidden);
#endif
    this->hidden = (struct SDL_PrivateAudioData *)0x1;

    /* limit to native freq */
    this->spec.freq = EM_ASM_INT_V({
      var SDL3 = Module['SDL3'];
      return SDL3.audioContext.sampleRate;
    });

    SDL_CalculateAudioSpec(&this->spec);

    if (iscapture) {
        /* The idea is to take the capture media stream, hook it up to an
           audio graph where we can pass it through a ScriptProcessorNode
           to access the raw PCM samples and push them to the SDL app's
           callback. From there, we "process" the audio data into silence
           and forget about it. */

        /* This should, strictly speaking, use MediaRecorder for capture, but
           this API is cleaner to use and better supported, and fires a
           callback whenever there's enough data to fire down into the app.
           The downside is that we are spending CPU time silencing a buffer
           that the audiocontext uselessly mixes into any output. On the
           upside, both of those things are not only run in native code in
           the browser, they're probably SIMD code, too. MediaRecorder
           feels like it's a pretty inefficient tapdance in similar ways,
           to be honest. */

        MAIN_THREAD_EM_ASM({
            var SDL3 = Module['SDL3'];
            var have_microphone = function(stream) {
                //console.log('SDL audio capture: we have a microphone! Replacing silence callback.');
                if (SDL3.capture.silenceTimer !== undefined) {
                    clearTimeout(SDL3.capture.silenceTimer);
                    SDL3.capture.silenceTimer = undefined;
                }
                SDL3.capture.mediaStreamNode = SDL3.audioContext.createMediaStreamSource(stream);
                SDL3.capture.scriptProcessorNode = SDL3.audioContext.createScriptProcessor($1, $0, 1);
                SDL3.capture.scriptProcessorNode.onaudioprocess = function(audioProcessingEvent) {
                    if ((SDL3 === undefined) || (SDL3.capture === undefined)) { return; }
                    audioProcessingEvent.outputBuffer.getChannelData(0).fill(0.0);
                    SDL3.capture.currentCaptureBuffer = audioProcessingEvent.inputBuffer;
                    dynCall('vi', $2, [$3]);
                };
                SDL3.capture.mediaStreamNode.connect(SDL3.capture.scriptProcessorNode);
                SDL3.capture.scriptProcessorNode.connect(SDL3.audioContext.destination);
                SDL3.capture.stream = stream;
            };

            var no_microphone = function(error) {
                //console.log('SDL audio capture: we DO NOT have a microphone! (' + error.name + ')...leaving silence callback running.');
            };

            /* we write silence to the audio callback until the microphone is available (user approves use, etc). */
            SDL3.capture.silenceBuffer = SDL3.audioContext.createBuffer($0, $1, SDL3.audioContext.sampleRate);
            SDL3.capture.silenceBuffer.getChannelData(0).fill(0.0);
            var silence_callback = function() {
                SDL3.capture.currentCaptureBuffer = SDL3.capture.silenceBuffer;
                dynCall('vi', $2, [$3]);
            };

            SDL3.capture.silenceTimer = setTimeout(silence_callback, ($1 / SDL3.audioContext.sampleRate) * 1000);

            if ((navigator.mediaDevices !== undefined) && (navigator.mediaDevices.getUserMedia !== undefined)) {
                navigator.mediaDevices.getUserMedia({ audio: true, video: false }).then(have_microphone).catch(no_microphone);
            } else if (navigator.webkitGetUserMedia !== undefined) {
                navigator.webkitGetUserMedia({ audio: true, video: false }, have_microphone, no_microphone);
            }
        }, this->spec.channels, this->spec.samples, HandleCaptureProcess, this);
    } else {
        /* setup a ScriptProcessorNode */
        MAIN_THREAD_EM_ASM({
            var SDL3 = Module['SDL3'];
            SDL3.audio.scriptProcessorNode = SDL3.audioContext['createScriptProcessor']($1, 0, $0);
            SDL3.audio.scriptProcessorNode['onaudioprocess'] = function (e) {
                if ((SDL3 === undefined) || (SDL3.audio === undefined)) { return; }
                SDL3.audio.currentOutputBuffer = e['outputBuffer'];
                dynCall('vi', $2, [$3]);
            };
            SDL3.audio.scriptProcessorNode['connect'](SDL3.audioContext['destination']);
        }, this->spec.channels, this->spec.samples, HandleAudioProcess, this);
    }

    return 0;
}

static void
EMSCRIPTENAUDIO_LockOrUnlockDeviceWithNoMixerLock(SDL_AudioDevice * device)
{
}

static SDL_bool
EMSCRIPTENAUDIO_Init(SDL_AudioDriverImpl * impl)
{
    SDL_bool available, capture_available;

    /* Set the function pointers */
    impl->OpenDevice = EMSCRIPTENAUDIO_OpenDevice;
    impl->CloseDevice = EMSCRIPTENAUDIO_CloseDevice;

    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;

    /* no threads here */
    impl->LockDevice = impl->UnlockDevice = EMSCRIPTENAUDIO_LockOrUnlockDeviceWithNoMixerLock;
    impl->ProvidesOwnCallbackThread = SDL_TRUE;

    /* check availability */
    available = MAIN_THREAD_EM_ASM_INT({
        if (typeof(AudioContext) !== 'undefined') {
            return true;
        } else if (typeof(webkitAudioContext) !== 'undefined') {
            return true;
        }
        return false;
    });

    if (!available) {
        SDL_SetError("No audio context available");
    }

    capture_available = available && MAIN_THREAD_EM_ASM_INT({
        if ((typeof(navigator.mediaDevices) !== 'undefined') && (typeof(navigator.mediaDevices.getUserMedia) !== 'undefined')) {
            return true;
        } else if (typeof(navigator.webkitGetUserMedia) !== 'undefined') {
            return true;
        }
        return false;
    });

    impl->HasCaptureSupport = capture_available ? SDL_TRUE : SDL_FALSE;
    impl->OnlyHasDefaultCaptureDevice = capture_available ? SDL_TRUE : SDL_FALSE;

    return available;
}

AudioBootStrap EMSCRIPTENAUDIO_bootstrap = {
    "emscripten", "SDL emscripten audio driver", EMSCRIPTENAUDIO_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_EMSCRIPTEN */

/* vi: set ts=4 sw=4 expandtab: */
