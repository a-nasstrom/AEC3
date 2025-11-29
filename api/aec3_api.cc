#include "aec3_api.h"
#include "echo_canceller3_factory.h"
#include "echo_canceller3_config.h"
#include "../audio_processing/include/audio_processing.h"
#include "../audio_processing/audio_buffer.h"
#include "../audio_processing/high_pass_filter.h"

using namespace webrtc;

struct aec3_handle {
    std::unique_ptr<EchoControl> echo_controller;
    std::unique_ptr<HighPassFilter> hp_filter;
    std::unique_ptr<AudioBuffer> ref_audio;
    std::unique_ptr<AudioBuffer> aec_audio;
    std::unique_ptr<AudioBuffer> aec_linear_audio;
    AudioFrame ref_frame;
    AudioFrame aec_frame;
    int sample_rate;
    int num_channels;
};

aec3_handle_t* aec3_create(const aec3_config_t* config) {
    if (!config) return nullptr;

    auto handle = new aec3_handle_t;
    handle->sample_rate = config->sample_rate;
    handle->num_channels = config->num_channels;

    EchoCanceller3Config aec_config;
    aec_config.filter.export_linear_aec_output = config->export_linear;
    
    // Configure suppression level (0.0 = minimal, 1.0 = maximum/aggressive)
    // Default to 1.0 (maximum suppression) if not specified
    float suppression_level = config->suppression_level > 0.0f ? config->suppression_level : 1.0f;
    suppression_level = std::max(0.0f, std::min(1.0f, suppression_level)); // Clamp to [0.0, 1.0]
    
    if (suppression_level >= 0.8f) {
        // Maximum/aggressive suppression settings
        // Lower thresholds for more aggressive suppression
        aec_config.suppressor.normal_tuning.mask_lf.enr_transparent = 0.1f;  // Lower = more suppression
        aec_config.suppressor.normal_tuning.mask_lf.enr_suppress = 0.2f;      // Lower = more suppression
        aec_config.suppressor.normal_tuning.mask_hf.enr_transparent = 0.05f;  // Lower = more suppression
        aec_config.suppressor.normal_tuning.mask_hf.enr_suppress = 0.08f;     // Lower = more suppression
        
        // Increase ERLE (Echo Return Loss Enhancement) for better echo cancellation
        aec_config.erle.max_l = 8.0f;  // Higher = better cancellation
        aec_config.erle.max_h = 4.0f;   // Higher = better cancellation
        
        // More aggressive filter settings
        aec_config.filter.main.leakage_converged = 0.00001f;  // Lower = faster adaptation
        aec_config.filter.main.error_floor = 0.0005f;         // Lower = better cancellation
        
        // Increase suppressor gain reduction during echo
        aec_config.suppressor.high_bands_suppression.max_gain_during_echo = 0.1f;  // Lower = more suppression
        aec_config.suppressor.high_bands_suppression.anti_howling_gain = 0.005f;  // Lower = more suppression
        
        // More sensitive echo detection
        aec_config.echo_audibility.audibility_threshold_lf = 5.0f;   // Lower = more sensitive
        aec_config.echo_audibility.audibility_threshold_mf = 5.0f;   // Lower = more sensitive
        aec_config.echo_audibility.audibility_threshold_hf = 5.0f;   // Lower = more sensitive
    } else if (suppression_level >= 0.5f) {
        // Medium suppression
        aec_config.suppressor.normal_tuning.mask_lf.enr_transparent = 0.2f;
        aec_config.suppressor.normal_tuning.mask_lf.enr_suppress = 0.3f;
        aec_config.suppressor.normal_tuning.mask_hf.enr_transparent = 0.06f;
        aec_config.suppressor.normal_tuning.mask_hf.enr_suppress = 0.09f;
        aec_config.erle.max_l = 6.0f;
        aec_config.erle.max_h = 3.0f;
        aec_config.suppressor.high_bands_suppression.max_gain_during_echo = 0.3f;
    }
    // For suppression_level < 0.5, use default settings (less aggressive)
    
    EchoCanceller3Factory aec_factory(aec_config);
    
    handle->echo_controller = aec_factory.Create(
        config->sample_rate, config->num_channels, config->num_channels);
    handle->hp_filter = std::make_unique<HighPassFilter>(
        config->sample_rate, config->num_channels);

    StreamConfig stream_config(config->sample_rate, config->num_channels, false);
    handle->ref_audio = std::make_unique<AudioBuffer>(
        stream_config.sample_rate_hz(), stream_config.num_channels(),
        stream_config.sample_rate_hz(), stream_config.num_channels(),
        stream_config.sample_rate_hz(), stream_config.num_channels());
    
    handle->aec_audio = std::make_unique<AudioBuffer>(
        stream_config.sample_rate_hz(), stream_config.num_channels(),
        stream_config.sample_rate_hz(), stream_config.num_channels(),
        stream_config.sample_rate_hz(), stream_config.num_channels());

    if (config->export_linear) {
        constexpr int kLinearOutputRateHz = 16000;
        handle->aec_linear_audio = std::make_unique<AudioBuffer>(
            kLinearOutputRateHz, stream_config.num_channels(),
            kLinearOutputRateHz, stream_config.num_channels(),
            kLinearOutputRateHz, stream_config.num_channels());
    }

    return handle;
}

int aec3_process_frame(
    aec3_handle_t* handle,
    const int16_t* reference_frame,
    const int16_t* capture_frame,
    int16_t* output_frame,
    int16_t* linear_output_frame,
    size_t frame_size,
    int buffer_delay) {
    
    if (!handle || !reference_frame || !capture_frame || !output_frame) {
        return -1;
    }

    // Update frames with new audio data
    handle->ref_frame.UpdateFrame(0, const_cast<int16_t*>(reference_frame),
        frame_size, handle->sample_rate,
        AudioFrame::kNormalSpeech, AudioFrame::kVadActive, 1);
    
    handle->aec_frame.UpdateFrame(0, const_cast<int16_t*>(capture_frame),
        frame_size, handle->sample_rate,
        AudioFrame::kNormalSpeech, AudioFrame::kVadActive, 1);

    // Copy frames to audio buffers
    handle->ref_audio->CopyFrom(&handle->ref_frame);
    handle->aec_audio->CopyFrom(&handle->aec_frame);

    // Process audio
    handle->ref_audio->SplitIntoFrequencyBands();
    handle->echo_controller->AnalyzeRender(handle->ref_audio.get());
    handle->ref_audio->MergeFrequencyBands();
    
    handle->echo_controller->AnalyzeCapture(handle->aec_audio.get());
    handle->aec_audio->SplitIntoFrequencyBands();
    handle->hp_filter->Process(handle->aec_audio.get(), true);
    
    // Use the buffer_delay parameter instead of hardcoded value
    handle->echo_controller->SetAudioBufferDelay(buffer_delay);
    
    if (handle->aec_linear_audio) {
        handle->echo_controller->ProcessCapture(
            handle->aec_audio.get(), handle->aec_linear_audio.get(), false);
    } else {
        handle->echo_controller->ProcessCapture(
            handle->aec_audio.get(), nullptr, false);
    }
    
    handle->aec_audio->MergeFrequencyBands();

    // Copy processed audio back to output
    handle->aec_audio->CopyTo(&handle->aec_frame);
    memcpy(output_frame, handle->aec_frame.data(),
        frame_size * handle->num_channels * sizeof(int16_t));

    // Copy linear output if requested
    if (linear_output_frame && handle->aec_linear_audio) {
        constexpr int kLinearOutputRateHz = 16000;
        handle->aec_frame.UpdateFrame(0, nullptr,
            kLinearOutputRateHz / 100, kLinearOutputRateHz,
            AudioFrame::kNormalSpeech, AudioFrame::kVadActive, 1);
        handle->aec_linear_audio->CopyTo(&handle->aec_frame);
        memcpy(linear_output_frame, handle->aec_frame.data(), 320);
    }

    return 0;
}

void aec3_destroy(aec3_handle_t* handle) {
    delete handle;
} 