/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_AUDIO_PROCESSING_INCLUDE_AUDIO_PROCESSING_H_
#define MODULES_AUDIO_PROCESSING_INCLUDE_AUDIO_PROCESSING_H_

// MSVC++ requires this to be set before any other includes to get M_PI.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include <math.h>
#include <stddef.h>  // size_t
#include <stdio.h>   // FILE
#include <string.h>
#include <vector>

#include "absl/types/optional.h"
#include "api/audio/echo_canceller3_config.h"
#include "api/audio/echo_control.h"
#include "modules/audio_processing/include/audio_generator.h"
#include "modules/audio_processing/include/audio_processing_statistics.h"
#include "modules/audio_processing/include/config.h"
#include "modules/audio_processing/include/gain_control.h"
#include "rtc_base/arraysize.h"
#include "rtc_base/deprecation.h"
#include "rtc_base/platform_file.h"
#include "rtc_base/refcount.h"
#include "rtc_base/scoped_ref_ptr.h"

namespace webrtc {

struct AecCore;

class AecDump;
class AudioBuffer;
class AudioFrame;

class StreamConfig;
class ProcessingConfig;

class EchoDetector;
class GainControl;
class HighPassFilter;
class LevelEstimator;
class NoiseSuppression;
class CustomAudioAnalyzer;
class CustomProcessing;
class VoiceDetection;

// Use to enable the extended filter mode in the AEC, along with robustness
// measures around the reported system delays. It comes with a significant
// increase in AEC complexity, but is much more robust to unreliable reported
// delays.
//
// Detailed changes to the algorithm:
// - The filter length is changed from 48 to 128 ms. This comes with tuning of
//   several parameters: i) filter adaptation stepsize and error threshold;
//   ii) non-linear processing smoothing and overdrive.
// - Option to ignore the reported delays on platforms which we deem
//   sufficiently unreliable. See WEBRTC_UNTRUSTED_DELAY in echo_cancellation.c.
// - Faster startup times by removing the excessive "startup phase" processing
//   of reported delays.
// - Much more conservative adjustments to the far-end read pointer. We smooth
//   the delay difference more heavily, and back off from the difference more.
//   Adjustments force a readaptation of the filter, so they should be avoided
//   except when really necessary.
struct ExtendedFilter {
  ExtendedFilter() : enabled(false) {}
  explicit ExtendedFilter(bool enabled) : enabled(enabled) {}
  static const ConfigOptionID identifier = ConfigOptionID::kExtendedFilter;
  bool enabled;
};

// Enables the refined linear filter adaptation in the echo canceller.
// This configuration only applies to non-mobile echo cancellation.
// It can be set in the constructor or using AudioProcessing::SetExtraOptions().
struct RefinedAdaptiveFilter {
  RefinedAdaptiveFilter() : enabled(false) {}
  explicit RefinedAdaptiveFilter(bool enabled) : enabled(enabled) {}
  static const ConfigOptionID identifier =
      ConfigOptionID::kAecRefinedAdaptiveFilter;
  bool enabled;
};

// Enables delay-agnostic echo cancellation. This feature relies on internally
// estimated delays between the process and reverse streams, thus not relying
// on reported system delays. This configuration only applies to non-mobile echo
// cancellation. It can be set in the constructor or using
// AudioProcessing::SetExtraOptions().
struct DelayAgnostic {
  DelayAgnostic() : enabled(false) {}
  explicit DelayAgnostic(bool enabled) : enabled(enabled) {}
  static const ConfigOptionID identifier = ConfigOptionID::kDelayAgnostic;
  bool enabled;
};

// Use to enable experimental gain control (AGC). At startup the experimental
// AGC moves the microphone volume up to |startup_min_volume| if the current
// microphone volume is set too low. The value is clamped to its operating range
// [12, 255]. Here, 255 maps to 100%.
//
// Must be provided through AudioProcessingBuilder().Create(config).
#if defined(WEBRTC_CHROMIUM_BUILD)
static const int kAgcStartupMinVolume = 85;
#else
static const int kAgcStartupMinVolume = 0;
#endif  // defined(WEBRTC_CHROMIUM_BUILD)
static constexpr int kClippedLevelMin = 70;
struct ExperimentalAgc {
  ExperimentalAgc() = default;
  explicit ExperimentalAgc(bool enabled) : enabled(enabled) {}
  ExperimentalAgc(bool enabled,
                  bool enabled_agc2_level_estimator,
                  bool digital_adaptive_disabled,
                  bool analyze_before_aec)
      : enabled(enabled),
        enabled_agc2_level_estimator(enabled_agc2_level_estimator),
        digital_adaptive_disabled(digital_adaptive_disabled),
        analyze_before_aec(analyze_before_aec) {}

  ExperimentalAgc(bool enabled, int startup_min_volume)
      : enabled(enabled), startup_min_volume(startup_min_volume) {}
  ExperimentalAgc(bool enabled, int startup_min_volume, int clipped_level_min)
      : enabled(enabled),
        startup_min_volume(startup_min_volume),
        clipped_level_min(clipped_level_min) {}
  static const ConfigOptionID identifier = ConfigOptionID::kExperimentalAgc;
  bool enabled = true;
  int startup_min_volume = kAgcStartupMinVolume;
  // Lowest microphone level that will be applied in response to clipping.
  int clipped_level_min = kClippedLevelMin;
  bool enabled_agc2_level_estimator = false;
  bool digital_adaptive_disabled = false;
  // 'analyze_before_aec' is an experimental flag. It is intended to be removed
  // at some point.
  bool analyze_before_aec = false;
};

// Use to enable experimental noise suppression. It can be set in the
// constructor or using AudioProcessing::SetExtraOptions().
struct ExperimentalNs {
  ExperimentalNs() : enabled(false) {}
  explicit ExperimentalNs(bool enabled) : enabled(enabled) {}
  static const ConfigOptionID identifier = ConfigOptionID::kExperimentalNs;
  bool enabled;
};

// The Audio Processing Module (APM) provides a collection of voice processing
// components designed for real-time communications software.
//
// APM operates on two audio streams on a frame-by-frame basis. Frames of the
// primary stream, on which all processing is applied, are passed to
// |ProcessStream()|. Frames of the reverse direction stream are passed to
// |ProcessReverseStream()|. On the client-side, this will typically be the
// near-end (capture) and far-end (render) streams, respectively. APM should be
// placed in the signal chain as close to the audio hardware abstraction layer
// (HAL) as possible.
//
// On the server-side, the reverse stream will normally not be used, with
// processing occurring on each incoming stream.
//
// Component interfaces follow a similar pattern and are accessed through
// corresponding getters in APM. All components are disabled at create-time,
// with default settings that are recommended for most situations. New settings
// can be applied without enabling a component. Enabling a component triggers
// memory allocation and initialization to allow it to start processing the
// streams.
//
// Thread safety is provided with the following assumptions to reduce locking
// overhead:
//   1. The stream getters and setters are called from the same thread as
//      ProcessStream(). More precisely, stream functions are never called
//      concurrently with ProcessStream().
//   2. Parameter getters are never called concurrently with the corresponding
//      setter.
//
// APM accepts only linear PCM audio data in chunks of 10 ms. The int16
// interfaces use interleaved data, while the float interfaces use deinterleaved
// data.
//
// Usage example, omitting error checking:
// AudioProcessing* apm = AudioProcessingBuilder().Create();
//
// AudioProcessing::Config config;
// config.echo_canceller.enabled = true;
// config.echo_canceller.mobile_mode = false;
// config.high_pass_filter.enabled = true;
// config.gain_controller2.enabled = true;
// apm->ApplyConfig(config)
//
// apm->noise_reduction()->set_level(kHighSuppression);
// apm->noise_reduction()->Enable(true);
//
// apm->gain_control()->set_analog_level_limits(0, 255);
// apm->gain_control()->set_mode(kAdaptiveAnalog);
// apm->gain_control()->Enable(true);
//
// apm->voice_detection()->Enable(true);
//
// // Start a voice call...
//
// // ... Render frame arrives bound for the audio HAL ...
// apm->ProcessReverseStream(render_frame);
//
// // ... Capture frame arrives from the audio HAL ...
// // Call required set_stream_ functions.
// apm->set_stream_delay_ms(delay_ms);
// apm->gain_control()->set_stream_analog_level(analog_level);
//
// apm->ProcessStream(capture_frame);
//
// // Call required stream_ functions.
// analog_level = apm->gain_control()->stream_analog_level();
// has_voice = apm->stream_has_voice();
//
// // Repeate render and capture processing for the duration of the call...
// // Start a new call...
// apm->Initialize();
//
// // Close the application...
// delete apm;
//
class AudioProcessing : public rtc::RefCountInterface {
 public:
  // The struct below constitutes the new parameter scheme for the audio
  // processing. It is being introduced gradually and until it is fully
  // introduced, it is prone to change.
  // TODO(peah): Remove this comment once the new config scheme is fully rolled
  // out.
  //
  // The parameters and behavior of the audio processing module are controlled
  // by changing the default values in the AudioProcessing::Config struct.
  // The config is applied by passing the struct to the ApplyConfig method.
  struct Config {
    // TODO(bugs.webrtc.org/9535): Currently unused. Use this to determine AEC.
    struct EchoCanceller {
      bool enabled = false;
      bool mobile_mode = false;
      // Recommended not to use. Will be removed in the future.
      // APM components are not fine-tuned for legacy suppression levels.
      bool legacy_moderate_suppression_level = false;
    } echo_canceller;

    struct ResidualEchoDetector {
      bool enabled = true;
    } residual_echo_detector;

    struct HighPassFilter {
      bool enabled = false;
    } high_pass_filter;

    // Enabled the pre-amplifier. It amplifies the capture signal
    // before any other processing is done.
    struct PreAmplifier {
      bool enabled = false;
      float fixed_gain_factor = 1.f;
    } pre_amplifier;

    // Enables the next generation AGC functionality. This feature replaces the
    // standard methods of gain control in the previous AGC. Enabling this
    // submodule enables an adaptive digital AGC followed by a limiter. By
    // setting |fixed_gain_db|, the limiter can be turned into a compressor that
    // first applies a fixed gain. The adaptive digital AGC can be turned off by
    // setting |adaptive_digital_mode=false|.
    struct GainController2 {
      bool enabled = false;
      bool adaptive_digital_mode = true;
      float fixed_gain_db = 0.f;
    } gain_controller2;

    // Explicit copy assignment implementation to avoid issues with memory
    // sanitizer complaints in case of self-assignment.
    // TODO(peah): Add buildflag to ensure that this is only included for memory
    // sanitizer builds.
    Config& operator=(const Config& config) {
      if (this != &config) {
        memcpy(this, &config, sizeof(*this));
      }
      return *this;
    }
  };

  // TODO(mgraczyk): Remove once all methods that use ChannelLayout are gone.
  enum ChannelLayout {
    kMono,
    // Left, right.
    kStereo,
    // Mono, keyboard, and mic.
    kMonoAndKeyboard,
    // Left, right, keyboard, and mic.
    kStereoAndKeyboard
  };

  // Specifies the properties of a setting to be passed to AudioProcessing at
  // runtime.
  class RuntimeSetting {
   public:
    enum class Type {
      kNotSpecified,
      kCapturePreGain,
      kCustomRenderProcessingRuntimeSetting
    };

    RuntimeSetting() : type_(Type::kNotSpecified), value_(0.f) {}
    ~RuntimeSetting() = default;

    static RuntimeSetting CreateCapturePreGain(float gain) {
      RTC_DCHECK_GE(gain, 1.f) << "Attenuation is not allowed.";
      return {Type::kCapturePreGain, gain};
    }

    static RuntimeSetting CreateCustomRenderSetting(float payload) {
      return {Type::kCustomRenderProcessingRuntimeSetting, payload};
    }

    Type type() const { return type_; }
    void GetFloat(float* value) const {
      RTC_DCHECK(value);
      *value = value_;
    }

   private:
    RuntimeSetting(Type id, float value) : type_(id), value_(value) {}
    Type type_;
    float value_;
  };

  ~AudioProcessing() override {}

  // Initializes internal states, while retaining all user settings. This
  // should be called before beginning to process a new audio stream. However,
  // it is not necessary to call before processing the first stream after
  // creation.
  //
  // It is also not necessary to call if the audio parameters (sample
  // rate and number of channels) have changed. Passing updated parameters
  // directly to |ProcessStream()| and |ProcessReverseStream()| is permissible.
  // If the parameters are known at init-time though, they may be provided.
  virtual int Initialize() = 0;

  // The int16 interfaces require:
  //   - only |NativeRate|s be used
  //   - that the input, output and reverse rates must match
  //   - that |processing_config.output_stream()| matches
  //     |processing_config.input_stream()|.
  //
  // The float interfaces accept arbitrary rates and support differing input and
  // output layouts, but the output must have either one channel or the same
  // number of channels as the input.
  virtual int Initialize(const ProcessingConfig& processing_config) = 0;

  // Initialize with unpacked parameters. See Initialize() above for details.
  //
  // TODO(mgraczyk): Remove once clients are updated to use the new interface.
  virtual int Initialize(int capture_input_sample_rate_hz,
                         int capture_output_sample_rate_hz,
                         int render_sample_rate_hz,
                         ChannelLayout capture_input_layout,
                         ChannelLayout capture_output_layout,
                         ChannelLayout render_input_layout) = 0;

  // TODO(peah): This method is a temporary solution used to take control
  // over the parameters in the audio processing module and is likely to change.
  virtual void ApplyConfig(const Config& config) = 0;

  // Pass down additional options which don't have explicit setters. This
  // ensures the options are applied immediately.
  virtual void SetExtraOptions(const webrtc::Config& config) = 0;

  // TODO(ajm): Only intended for internal use. Make private and friend the
  // necessary classes?
  virtual int proc_sample_rate_hz() const = 0;
  virtual int proc_split_sample_rate_hz() const = 0;
  virtual size_t num_input_channels() const = 0;
  virtual size_t num_proc_channels() const = 0;
  virtual size_t num_output_channels() const = 0;
  virtual size_t num_reverse_channels() const = 0;

  // Set to true when the output of AudioProcessing will be muted or in some
  // other way not used. Ideally, the captured audio would still be processed,
  // but some components may change behavior based on this information.
  // Default false.
  virtual void set_output_will_be_muted(bool muted) = 0;

  // Enqueue a runtime setting.
  virtual void SetRuntimeSetting(RuntimeSetting setting) = 0;

  // Processes a 10 ms |frame| of the primary audio stream. On the client-side,
  // this is the near-end (or captured) audio.
  //
  // If needed for enabled functionality, any function with the set_stream_ tag
  // must be called prior to processing the current frame. Any getter function
  // with the stream_ tag which is needed should be called after processing.
  //
  // The |sample_rate_hz_|, |num_channels_|, and |samples_per_channel_|
  // members of |frame| must be valid. If changed from the previous call to this
  // method, it will trigger an initialization.
  virtual int ProcessStream(AudioFrame* frame) = 0;

  // Accepts deinterleaved float audio with the range [-1, 1]. Each element
  // of |src| points to a channel buffer, arranged according to
  // |input_layout|. At output, the channels will be arranged according to
  // |output_layout| at |output_sample_rate_hz| in |dest|.
  //
  // The output layout must have one channel or as many channels as the input.
  // |src| and |dest| may use the same memory, if desired.
  //
  // TODO(mgraczyk): Remove once clients are updated to use the new interface.
  virtual int ProcessStream(const float* const* src,
                            size_t samples_per_channel,
                            int input_sample_rate_hz,
                            ChannelLayout input_layout,
                            int output_sample_rate_hz,
                            ChannelLayout output_layout,
                            float* const* dest) = 0;

  // Accepts deinterleaved float audio with the range [-1, 1]. Each element of
  // |src| points to a channel buffer, arranged according to |input_stream|. At
  // output, the channels will be arranged according to |output_stream| in
  // |dest|.
  //
  // The output must have one channel or as many channels as the input. |src|
  // and |dest| may use the same memory, if desired.
  virtual int ProcessStream(const float* const* src,
                            const StreamConfig& input_config,
                            const StreamConfig& output_config,
                            float* const* dest) = 0;

  // Processes a 10 ms |frame| of the reverse direction audio stream. The frame
  // may be modified. On the client-side, this is the far-end (or to be
  // rendered) audio.
  //
  // It is necessary to provide this if echo processing is enabled, as the
  // reverse stream forms the echo reference signal. It is recommended, but not
  // necessary, to provide if gain control is enabled. On the server-side this
  // typically will not be used. If you're not sure what to pass in here,
  // chances are you don't need to use it.
  //
  // The |sample_rate_hz_|, |num_channels_|, and |samples_per_channel_|
  // members of |frame| must be valid.
  virtual int ProcessReverseStream(AudioFrame* frame) = 0;

  // Accepts deinterleaved float audio with the range [-1, 1]. Each element
  // of |data| points to a channel buffer, arranged according to |layout|.
  // TODO(mgraczyk): Remove once clients are updated to use the new interface.
  virtual int AnalyzeReverseStream(const float* const* data,
                                   size_t samples_per_channel,
                                   int sample_rate_hz,
                                   ChannelLayout layout) = 0;

  // Accepts deinterleaved float audio with the range [-1, 1]. Each element of
  // |data| points to a channel buffer, arranged according to |reverse_config|.
  virtual int ProcessReverseStream(const float* const* src,
                                   const StreamConfig& input_config,
                                   const StreamConfig& output_config,
                                   float* const* dest) = 0;

  // This must be called if and only if echo processing is enabled.
  //
  // Sets the |delay| in ms between ProcessReverseStream() receiving a far-end
  // frame and ProcessStream() receiving a near-end frame containing the
  // corresponding echo. On the client-side this can be expressed as
  //   delay = (t_render - t_analyze) + (t_process - t_capture)
  // where,
  //   - t_analyze is the time a frame is passed to ProcessReverseStream() and
  //     t_render is the time the first sample of the same frame is rendered by
  //     the audio hardware.
  //   - t_capture is the time the first sample of a frame is captured by the
  //     audio hardware and t_process is the time the same frame is passed to
  //     ProcessStream().
  virtual int set_stream_delay_ms(int delay) = 0;
  virtual int stream_delay_ms() const = 0;
  virtual bool was_stream_delay_set() const = 0;

  // Call to signal that a key press occurred (true) or did not occur (false)
  // with this chunk of audio.
  virtual void set_stream_key_pressed(bool key_pressed) = 0;

  // Sets a delay |offset| in ms to add to the values passed in through
  // set_stream_delay_ms(). May be positive or negative.
  //
  // Note that this could cause an otherwise valid value passed to
  // set_stream_delay_ms() to return an error.
  virtual void set_delay_offset_ms(int offset) = 0;
  virtual int delay_offset_ms() const = 0;

  // Attaches provided webrtc::AecDump for recording debugging
  // information. Log file and maximum file size logic is supposed to
  // be handled by implementing instance of AecDump. Calling this
  // method when another AecDump is attached resets the active AecDump
  // with a new one. This causes the d-tor of the earlier AecDump to
  // be called. The d-tor call may block until all pending logging
  // tasks are completed.
  virtual void AttachAecDump(std::unique_ptr<AecDump> aec_dump) = 0;

  // If no AecDump is attached, this has no effect. If an AecDump is
  // attached, it's destructor is called. The d-tor may block until
  // all pending logging tasks are completed.
  virtual void DetachAecDump() = 0;

  // Attaches provided webrtc::AudioGenerator for modifying playout audio.
  // Calling this method when another AudioGenerator is attached replaces the
  // active AudioGenerator with a new one.
  virtual void AttachPlayoutAudioGenerator(
      std::unique_ptr<AudioGenerator> audio_generator) = 0;

  // If no AudioGenerator is attached, this has no effect. If an AecDump is
  // attached, its destructor is called.
  virtual void DetachPlayoutAudioGenerator() = 0;

  // Use to send UMA histograms at end of a call. Note that all histogram
  // specific member variables are reset.
  virtual void UpdateHistogramsOnCallEnd() = 0;

  // TODO(ivoc): Remove when the calling code no longer uses the old Statistics
  //             API.
  struct Statistic {
    int instant = 0;  // Instantaneous value.
    int average = 0;  // Long-term average.
    int maximum = 0;  // Long-term maximum.
    int minimum = 0;  // Long-term minimum.
  };

  struct Stat {
    void Set(const Statistic& other) {
      Set(other.instant, other.average, other.maximum, other.minimum);
    }
    void Set(float instant, float average, float maximum, float minimum) {
      instant_ = instant;
      average_ = average;
      maximum_ = maximum;
      minimum_ = minimum;
    }
    float instant() const { return instant_; }
    float average() const { return average_; }
    float maximum() const { return maximum_; }
    float minimum() const { return minimum_; }

   private:
    float instant_ = 0.0f;  // Instantaneous value.
    float average_ = 0.0f;  // Long-term average.
    float maximum_ = 0.0f;  // Long-term maximum.
    float minimum_ = 0.0f;  // Long-term minimum.
  };

  struct AudioProcessingStatistics {
    AudioProcessingStatistics();
    AudioProcessingStatistics(const AudioProcessingStatistics& other);
    ~AudioProcessingStatistics();

    // AEC Statistics.
    // RERL = ERL + ERLE
    Stat residual_echo_return_loss;
    // ERL = 10log_10(P_far / P_echo)
    Stat echo_return_loss;
    // ERLE = 10log_10(P_echo / P_out)
    Stat echo_return_loss_enhancement;
    // (Pre non-linear processing suppression) A_NLP = 10log_10(P_echo / P_a)
    Stat a_nlp;
    // Fraction of time that the AEC linear filter is divergent, in a 1-second
    // non-overlapped aggregation window.
    float divergent_filter_fraction = -1.0f;

    // The delay metrics consists of the delay median and standard deviation. It
    // also consists of the fraction of delay estimates that can make the echo
    // cancellation perform poorly. The values are aggregated until the first
    // call to |GetStatistics()| and afterwards aggregated and updated every
    // second. Note that if there are several clients pulling metrics from
    // |GetStatistics()| during a session the first call from any of them will
    // change to one second aggregation window for all.
    int delay_median = -1;
    int delay_standard_deviation = -1;
    float fraction_poor_delays = -1.0f;

    // Residual echo detector likelihood.
    float residual_echo_likelihood = -1.0f;
    // Maximum residual echo likelihood from the last time period.
    float residual_echo_likelihood_recent_max = -1.0f;
  };

  // TODO(ivoc): Make this pure virtual when all subclasses have been updated.
  virtual AudioProcessingStatistics GetStatistics() const;

  // This returns the stats as optionals and it will replace the regular
  // GetStatistics.
  virtual AudioProcessingStats GetStatistics(bool has_remote_tracks) const;

  // These provide access to the component interfaces and should never return
  // NULL. The pointers will be valid for the lifetime of the APM instance.
  // The memory for these objects is entirely managed internally.
  virtual GainControl* gain_control() const = 0;
  // TODO(peah): Deprecate this API call.
  virtual HighPassFilter* high_pass_filter() const = 0;
  virtual LevelEstimator* level_estimator() const = 0;
  virtual NoiseSuppression* noise_suppression() const = 0;
  virtual VoiceDetection* voice_detection() const = 0;

  // Returns the last applied configuration.
  virtual AudioProcessing::Config GetConfig() const = 0;

  enum Error {
    // Fatal errors.
    kNoError = 0,
    kUnspecifiedError = -1,
    kCreationFailedError = -2,
    kUnsupportedComponentError = -3,
    kUnsupportedFunctionError = -4,
    kNullPointerError = -5,
    kBadParameterError = -6,
    kBadSampleRateError = -7,
    kBadDataLengthError = -8,
    kBadNumberChannelsError = -9,
    kFileError = -10,
    kStreamParameterNotSetError = -11,
    kNotEnabledError = -12,

    // Warnings are non-fatal.
    // This results when a set_stream_ parameter is out of range. Processing
    // will continue, but the parameter may have been truncated.
    kBadStreamParameterWarning = -13
  };

  enum NativeRate {
    kSampleRate8kHz = 8000,
    kSampleRate16kHz = 16000,
    kSampleRate32kHz = 32000,
    kSampleRate48kHz = 48000
  };

  // TODO(kwiberg): We currently need to support a compiler (Visual C++) that
  // complains if we don't explicitly state the size of the array here. Remove
  // the size when that's no longer the case.
  static constexpr int kNativeSampleRatesHz[4] = {
      kSampleRate8kHz, kSampleRate16kHz, kSampleRate32kHz, kSampleRate48kHz};
  static constexpr size_t kNumNativeSampleRates =
      arraysize(kNativeSampleRatesHz);
  static constexpr int kMaxNativeSampleRateHz =
      kNativeSampleRatesHz[kNumNativeSampleRates - 1];

  static const int kChunkSizeMs = 10;
};

class AudioProcessingBuilder {
 public:
  AudioProcessingBuilder();
  ~AudioProcessingBuilder();
  // The AudioProcessingBuilder takes ownership of the echo_control_factory.
  AudioProcessingBuilder& SetEchoControlFactory(
      std::unique_ptr<EchoControlFactory> echo_control_factory);
  // The AudioProcessingBuilder takes ownership of the capture_post_processing.
  AudioProcessingBuilder& SetCapturePostProcessing(
      std::unique_ptr<CustomProcessing> capture_post_processing);
  // The AudioProcessingBuilder takes ownership of the render_pre_processing.
  AudioProcessingBuilder& SetRenderPreProcessing(
      std::unique_ptr<CustomProcessing> render_pre_processing);
  // The AudioProcessingBuilder takes ownership of the echo_detector.
  AudioProcessingBuilder& SetEchoDetector(
      rtc::scoped_refptr<EchoDetector> echo_detector);
  // The AudioProcessingBuilder takes ownership of the capture_analyzer.
  AudioProcessingBuilder& SetCaptureAnalyzer(
      std::unique_ptr<CustomAudioAnalyzer> capture_analyzer);
  // This creates an APM instance using the previously set components. Calling
  // the Create function resets the AudioProcessingBuilder to its initial state.
  AudioProcessing* Create();
  AudioProcessing* Create(const webrtc::Config& config);

 private:
  std::unique_ptr<EchoControlFactory> echo_control_factory_;
  std::unique_ptr<CustomProcessing> capture_post_processing_;
  std::unique_ptr<CustomProcessing> render_pre_processing_;
  rtc::scoped_refptr<EchoDetector> echo_detector_;
  std::unique_ptr<CustomAudioAnalyzer> capture_analyzer_;
  RTC_DISALLOW_COPY_AND_ASSIGN(AudioProcessingBuilder);
};

class StreamConfig {
 public:
  // sample_rate_hz: The sampling rate of the stream.
  //
  // num_channels: The number of audio channels in the stream, excluding the
  //               keyboard channel if it is present. When passing a
  //               StreamConfig with an array of arrays T*[N],
  //
  //                N == {num_channels + 1  if  has_keyboard
  //                     {num_channels      if  !has_keyboard
  //
  // has_keyboard: True if the stream has a keyboard channel. When has_keyboard
  //               is true, the last channel in any corresponding list of
  //               channels is the keyboard channel.
  StreamConfig(int sample_rate_hz = 0,
               size_t num_channels = 0,
               bool has_keyboard = false)
      : sample_rate_hz_(sample_rate_hz),
        num_channels_(num_channels),
        has_keyboard_(has_keyboard),
        num_frames_(calculate_frames(sample_rate_hz)) {}

  void set_sample_rate_hz(int value) {
    sample_rate_hz_ = value;
    num_frames_ = calculate_frames(value);
  }
  void set_num_channels(size_t value) { num_channels_ = value; }
  void set_has_keyboard(bool value) { has_keyboard_ = value; }

  int sample_rate_hz() const { return sample_rate_hz_; }

  // The number of channels in the stream, not including the keyboard channel if
  // present.
  size_t num_channels() const { return num_channels_; }

  bool has_keyboard() const { return has_keyboard_; }
  size_t num_frames() const { return num_frames_; }
  size_t num_samples() const { return num_channels_ * num_frames_; }

  bool operator==(const StreamConfig& other) const {
    return sample_rate_hz_ == other.sample_rate_hz_ &&
           num_channels_ == other.num_channels_ &&
           has_keyboard_ == other.has_keyboard_;
  }

  bool operator!=(const StreamConfig& other) const { return !(*this == other); }

 private:
  static size_t calculate_frames(int sample_rate_hz) {
    return static_cast<size_t>(AudioProcessing::kChunkSizeMs * sample_rate_hz /
                               1000);
  }

  int sample_rate_hz_;
  size_t num_channels_;
  bool has_keyboard_;
  size_t num_frames_;
};

class ProcessingConfig {
 public:
  enum StreamName {
    kInputStream,
    kOutputStream,
    kReverseInputStream,
    kReverseOutputStream,
    kNumStreamNames,
  };

  const StreamConfig& input_stream() const {
    return streams[StreamName::kInputStream];
  }
  const StreamConfig& output_stream() const {
    return streams[StreamName::kOutputStream];
  }
  const StreamConfig& reverse_input_stream() const {
    return streams[StreamName::kReverseInputStream];
  }
  const StreamConfig& reverse_output_stream() const {
    return streams[StreamName::kReverseOutputStream];
  }

  StreamConfig& input_stream() { return streams[StreamName::kInputStream]; }
  StreamConfig& output_stream() { return streams[StreamName::kOutputStream]; }
  StreamConfig& reverse_input_stream() {
    return streams[StreamName::kReverseInputStream];
  }
  StreamConfig& reverse_output_stream() {
    return streams[StreamName::kReverseOutputStream];
  }

  bool operator==(const ProcessingConfig& other) const {
    for (int i = 0; i < StreamName::kNumStreamNames; ++i) {
      if (this->streams[i] != other.streams[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const ProcessingConfig& other) const {
    return !(*this == other);
  }

  StreamConfig streams[StreamName::kNumStreamNames];
};

// TODO(peah): Remove this interface.
// A filtering component which removes DC offset and low-frequency noise.
// Recommended to be enabled on the client-side.
class HighPassFilter {
 public:
  virtual int Enable(bool enable) = 0;
  virtual bool is_enabled() const = 0;

  virtual ~HighPassFilter() {}
};

// An estimation component used to retrieve level metrics.
class LevelEstimator {
 public:
  virtual int Enable(bool enable) = 0;
  virtual bool is_enabled() const = 0;

  // Returns the root mean square (RMS) level in dBFs (decibels from digital
  // full-scale), or alternately dBov. It is computed over all primary stream
  // frames since the last call to RMS(). The returned value is positive but
  // should be interpreted as negative. It is constrained to [0, 127].
  //
  // The computation follows: https://tools.ietf.org/html/rfc6465
  // with the intent that it can provide the RTP audio level indication.
  //
  // Frames passed to ProcessStream() with an |_energy| of zero are considered
  // to have been muted. The RMS of the frame will be interpreted as -127.
  virtual int RMS() = 0;

 protected:
  virtual ~LevelEstimator() {}
};

// The noise suppression (NS) component attempts to remove noise while
// retaining speech. Recommended to be enabled on the client-side.
//
// Recommended to be enabled on the client-side.
class NoiseSuppression {
 public:
  virtual int Enable(bool enable) = 0;
  virtual bool is_enabled() const = 0;

  // Determines the aggressiveness of the suppression. Increasing the level
  // will reduce the noise level at the expense of a higher speech distortion.
  enum Level { kLow, kModerate, kHigh, kVeryHigh };

  virtual int set_level(Level level) = 0;
  virtual Level level() const = 0;

  // Returns the internally computed prior speech probability of current frame
  // averaged over output channels. This is not supported in fixed point, for
  // which |kUnsupportedFunctionError| is returned.
  virtual float speech_probability() const = 0;

  // Returns the noise estimate per frequency bin averaged over all channels.
  virtual std::vector<float> NoiseEstimate() = 0;

 protected:
  virtual ~NoiseSuppression() {}
};

// Experimental interface for a custom analysis submodule.
class CustomAudioAnalyzer {
 public:
  // (Re-) Initializes the submodule.
  virtual void Initialize(int sample_rate_hz, int num_channels) = 0;
  // Analyzes the given capture or render signal.
  virtual void Analyze(const AudioBuffer* audio) = 0;
  // Returns a string representation of the module state.
  virtual std::string ToString() const = 0;

  virtual ~CustomAudioAnalyzer() {}
};

// Interface for a custom processing submodule.
class CustomProcessing {
 public:
  // (Re-)Initializes the submodule.
  virtual void Initialize(int sample_rate_hz, int num_channels) = 0;
  // Processes the given capture or render signal.
  virtual void Process(AudioBuffer* audio) = 0;
  // Returns a string representation of the module state.
  virtual std::string ToString() const = 0;
  // Handles RuntimeSettings. TODO(webrtc:9262): make pure virtual
  // after updating dependencies.
  virtual void SetRuntimeSetting(AudioProcessing::RuntimeSetting setting);

  virtual ~CustomProcessing() {}
};

// Interface for an echo detector submodule.
class EchoDetector : public rtc::RefCountInterface {
 public:
  // (Re-)Initializes the submodule.
  virtual void Initialize(int capture_sample_rate_hz,
                          int num_capture_channels,
                          int render_sample_rate_hz,
                          int num_render_channels) = 0;

  // Analysis (not changing) of the render signal.
  virtual void AnalyzeRenderAudio(rtc::ArrayView<const float> render_audio) = 0;

  // Analysis (not changing) of the capture signal.
  virtual void AnalyzeCaptureAudio(
      rtc::ArrayView<const float> capture_audio) = 0;

  // Pack an AudioBuffer into a vector<float>.
  static void PackRenderAudioBuffer(AudioBuffer* audio,
                                    std::vector<float>* packed_buffer);

  struct Metrics {
    double echo_likelihood;
    double echo_likelihood_recent_max;
  };

  // Collect current metrics from the echo detector.
  virtual Metrics GetMetrics() const = 0;
};

// The voice activity detection (VAD) component analyzes the stream to
// determine if voice is present. A facility is also provided to pass in an
// external VAD decision.
//
// In addition to |stream_has_voice()| the VAD decision is provided through the
// |AudioFrame| passed to |ProcessStream()|. The |vad_activity_| member will be
// modified to reflect the current decision.
class VoiceDetection {
 public:
  virtual int Enable(bool enable) = 0;
  virtual bool is_enabled() const = 0;

  // Returns true if voice is detected in the current frame. Should be called
  // after |ProcessStream()|.
  virtual bool stream_has_voice() const = 0;

  // Some of the APM functionality requires a VAD decision. In the case that
  // a decision is externally available for the current frame, it can be passed
  // in here, before |ProcessStream()| is called.
  //
  // VoiceDetection does _not_ need to be enabled to use this. If it happens to
  // be enabled, detection will be skipped for any frame in which an external
  // VAD decision is provided.
  virtual int set_stream_has_voice(bool has_voice) = 0;

  // Specifies the likelihood that a frame will be declared to contain voice.
  // A higher value makes it more likely that speech will not be clipped, at
  // the expense of more noise being detected as voice.
  enum Likelihood {
    kVeryLowLikelihood,
    kLowLikelihood,
    kModerateLikelihood,
    kHighLikelihood
  };

  virtual int set_likelihood(Likelihood likelihood) = 0;
  virtual Likelihood likelihood() const = 0;

  // Sets the |size| of the frames in ms on which the VAD will operate. Larger
  // frames will improve detection accuracy, but reduce the frequency of
  // updates.
  //
  // This does not impact the size of frames passed to |ProcessStream()|.
  virtual int set_frame_size_ms(int size) = 0;
  virtual int frame_size_ms() const = 0;

 protected:
  virtual ~VoiceDetection() {}
};

}  // namespace webrtc

#endif  // MODULES_AUDIO_PROCESSING_INCLUDE_AUDIO_PROCESSING_H_
