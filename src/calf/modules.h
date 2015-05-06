/* Calf DSP plugin pack
 * Assorted plugins
 *
 * Copyright (C) 2001-2010 Krzysztof Foltman, Markus Schmidt, Thor Harald Johansen and others
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, 
 * Boston, MA 02111-1307, USA.
 */
#ifndef CALF_MODULES_H
#define CALF_MODULES_H

#include <assert.h>
#include <fftw3.h>
#include <limits.h>
#include "biquad.h"
#include "inertia.h"
#include "audio_fx.h"
#include "giface.h"
#include "metadata.h"
#include "loudness.h"

namespace calf_plugins {

struct ladspa_plugin_info;

class reverb_audio_module: public audio_module<reverb_metadata>
{
public:    
    dsp::reverb reverb;
    dsp::simple_delay<16384, dsp::stereo_sample<float> > pre_delay;
    dsp::onepole<float> left_lo, right_lo, left_hi, right_hi;
    uint32_t srate;
    dsp::gain_smoothing amount, dryamount;
    int predelay_amt;
    float meter_wet, meter_out;
    uint32_t clip;
    
    void params_changed();
    uint32_t process(uint32_t offset, uint32_t numsamples, uint32_t inputs_mask, uint32_t outputs_mask);
    void activate();
    void set_sample_rate(uint32_t sr);
    void deactivate();
};

class vintage_delay_audio_module: public audio_module<vintage_delay_metadata>
{
public:    
    // 1MB of delay memory per channel... uh, RAM is cheap
    enum { MAX_DELAY = 262144, ADDR_MASK = MAX_DELAY - 1 };
    enum { MIXMODE_STEREO, MIXMODE_PINGPONG, MIXMODE_LR, MIXMODE_RL }; 
    float buffers[2][MAX_DELAY];
    int bufptr, deltime_l, deltime_r, mixmode, medium, old_medium;
    /// number of table entries written (value is only important when it is less than MAX_DELAY, which means that the buffer hasn't been totally filled yet)
    int age;
    
    dsp::gain_smoothing amt_left, amt_right, fb_left, fb_right, dry, chmix;
    
    dsp::biquad_d2<float> biquad_left[2], biquad_right[2];
    
    uint32_t srate;
    
    vintage_delay_audio_module();
    
    void params_changed();
    void activate();
    void deactivate();
    void set_sample_rate(uint32_t sr);
    void calc_filters();
    uint32_t process(uint32_t offset, uint32_t numsamples, uint32_t inputs_mask, uint32_t outputs_mask);
    
    long _tap_avg;
    long _tap_last;
};

template<typename FilterClass, typename Metadata>
class filter_module_with_inertia: public audio_module<Metadata>, public FilterClass
{
public:
    /// These are pointers to the ins, outs, params arrays in the main class
    typedef filter_module_with_inertia inertia_filter_module;
    using audio_module<Metadata>::ins;
    using audio_module<Metadata>::outs;
    using audio_module<Metadata>::params;
    
    dsp::inertia<dsp::exponential_ramp> inertia_cutoff, inertia_resonance, inertia_gain;
    dsp::once_per_n timer;
    bool is_active;    
    mutable volatile int last_generation, last_calculated_generation;
    
    filter_module_with_inertia(float **ins, float **outs, float **params)
    : inertia_cutoff(dsp::exponential_ramp(128), 20)
    , inertia_resonance(dsp::exponential_ramp(128), 20)
    , inertia_gain(dsp::exponential_ramp(128), 1.0)
    , timer(128)
    , is_active(false)
    , last_generation(-1)
    , last_calculated_generation(-2)
    {}
    
    void calculate_filter()
    {
        float freq = inertia_cutoff.get_last();
        // printf("freq=%g inr.cnt=%d timer.left=%d\n", freq, inertia_cutoff.count, timer.left);
        // XXXKF this is resonance of a single stage, obviously for three stages, resonant gain will be different
        float q    = inertia_resonance.get_last();
        int   mode = dsp::fastf2i_drm(*params[Metadata::par_mode]);
        // printf("freq = %f q = %f mode = %d\n", freq, q, mode);
        
        int inertia = dsp::fastf2i_drm(*params[Metadata::par_inertia]);
        if (inertia != inertia_cutoff.ramp.length()) {
            inertia_cutoff.ramp.set_length(inertia);
            inertia_resonance.ramp.set_length(inertia);
            inertia_gain.ramp.set_length(inertia);
        }
        
        FilterClass::calculate_filter(freq, q, mode, inertia_gain.get_last());
    }
    
    virtual void params_changed()
    {
        calculate_filter();
    }
    
    void on_timer()
    {
        int gen = last_generation;
        inertia_cutoff.step();
        inertia_resonance.step();
        inertia_gain.step();
        calculate_filter();
        last_calculated_generation = gen;
    }
    
    void activate()
    {
        params_changed();
        FilterClass::filter_activate();
        timer = dsp::once_per_n(FilterClass::srate / 1000);
        timer.start();
        is_active = true;
    }
    
    void set_sample_rate(uint32_t sr)
    {
        FilterClass::srate = sr;
    }

    
    void deactivate()
    {
        is_active = false;
    }

    uint32_t process(uint32_t offset, uint32_t numsamples, uint32_t inputs_mask, uint32_t outputs_mask) {
//        printf("sr=%d cutoff=%f res=%f mode=%f\n", FilterClass::srate, *params[Metadata::par_cutoff], *params[Metadata::par_resonance], *params[Metadata::par_mode]);
        uint32_t ostate = 0;
        numsamples += offset;
        while(offset < numsamples) {
            uint32_t numnow = numsamples - offset;
            // if inertia's inactive, we can calculate the whole buffer at once
            if (inertia_cutoff.active() || inertia_resonance.active() || inertia_gain.active())
                numnow = timer.get(numnow);
            
            if (outputs_mask & 1) {
                ostate |= FilterClass::process_channel(0, ins[0] + offset, outs[0] + offset, numnow, inputs_mask & 1);
            }
            if (outputs_mask & 2) {
                ostate |= FilterClass::process_channel(1, ins[1] + offset, outs[1] + offset, numnow, inputs_mask & 2);
            }
            
            if (timer.elapsed()) {
                on_timer();
            }
            offset += numnow;
        }
        return ostate;
    }
};

/// biquad filter module
class filter_audio_module: 
    public filter_module_with_inertia<dsp::biquad_filter_module, filter_metadata>, 
    public frequency_response_line_graph
{
    mutable float old_cutoff, old_resonance, old_mode;
public:    
    filter_audio_module()
    : filter_module_with_inertia<dsp::biquad_filter_module, filter_metadata>(ins, outs, params)
    {
        last_generation = 0;
        old_mode = old_resonance = old_cutoff = -1;
    }
    void params_changed()
    { 
        inertia_cutoff.set_inertia(*params[par_cutoff]);
        inertia_resonance.set_inertia(*params[par_resonance]);
        inertia_filter_module::params_changed(); 
    }
        
    bool get_graph(int index, int subindex, float *data, int points, cairo_iface *context, int *mode) const;
    int get_changed_offsets(int index, int generation, int &subindex_graph, int &subindex_dot, int &subindex_gridline) const;
};

/// Filterclavier --- MIDI controlled filter by Hans Baier
class filterclavier_audio_module: 
        public filter_module_with_inertia<dsp::biquad_filter_module, filterclavier_metadata>, 
        public frequency_response_line_graph
{        
    using audio_module<filterclavier_metadata>::ins;
    using audio_module<filterclavier_metadata>::outs;
    using audio_module<filterclavier_metadata>::params;

    const float min_gain;
    const float max_gain;
    
    int last_note;
    int last_velocity;
        
public:    
    filterclavier_audio_module();
    void params_changed();
    void activate();
    void set_sample_rate(uint32_t sr);
    void deactivate();
  
    /// MIDI control
    virtual void note_on(int channel, int note, int vel);
    virtual void note_off(int channel, int note, int vel);
    
    bool get_graph(int index, int subindex, float *data, int points, cairo_iface *context, int *mode) const;
    
private:
    void adjust_gain_according_to_filter_mode(int velocity);
};


#define MATH_E 2.718281828
class mono_audio_module:
    public audio_module<mono_metadata>
{
    typedef mono_audio_module AM;
    uint32_t srate;
    bool active;
    
    uint32_t clip_in, clip_outL, clip_outR;
    float meter_in, meter_outL, meter_outR;
    
    float * buffer;
    unsigned int pos;
    unsigned int buffer_size;
    float sign(float x) {
        if(x < 0) return -1.f;
        if(x > 0) return 1.f;
        return 0.f;
    }
    float _phase, _phase_sin_coef, _phase_cos_coef, _sc_level, _inv_atan_shape;
public:
    mono_audio_module();
    void params_changed();
    void activate();
    void set_sample_rate(uint32_t sr);
    void deactivate();
    uint32_t process(uint32_t offset, uint32_t numsamples, uint32_t inputs_mask, uint32_t outputs_mask);
};

class stereo_audio_module:
    public audio_module<stereo_metadata>
{
    typedef stereo_audio_module AM;
    float LL, LR, RL, RR;
    uint32_t srate;
    bool active;
    
    uint32_t clip_inL, clip_inR, clip_outL, clip_outR;
    float meter_inL, meter_inR, meter_outL, meter_outR, meter_phase;
    
    float * buffer;
    unsigned int pos;
    unsigned int buffer_size;
    float sign(float x) {
        if(x < 0) return -1.f;
        if(x > 0) return 1.f;
        return 0.f;
    }
    float _phase, _phase_sin_coef, _phase_cos_coef, _sc_level, _inv_atan_shape;
public:
    stereo_audio_module();
    void params_changed();
    void activate();
    void set_sample_rate(uint32_t sr);
    void deactivate();
    uint32_t process(uint32_t offset, uint32_t numsamples, uint32_t inputs_mask, uint32_t outputs_mask);
};

class analyzer_audio_module:
    public audio_module<analyzer_metadata>, public frequency_response_line_graph, public phase_graph_iface
{
    typedef analyzer_audio_module AM;
    uint32_t srate;
    bool active;
    int _accuracy;
    int _acc_old;
    int _scale_old;
    int _post_old;
    int _hold_old;
    int _smooth_old;
    uint32_t clip_L, clip_R;
    float meter_L, meter_R;
    
public:
    analyzer_audio_module();
    void params_changed();
    void activate();
    void set_sample_rate(uint32_t sr);
    void deactivate();
    uint32_t process(uint32_t offset, uint32_t numsamples, uint32_t inputs_mask, uint32_t outputs_mask);
    bool get_phase_graph(float ** _buffer, int * _length, int * _mode, bool * _use_fade, float * _fade, int * _accuracy, bool * _display) const;
    bool get_graph(int index, int subindex, float *data, int points, cairo_iface *context, int *mode) const;
    bool get_gridline(int index, int subindex, float &pos, bool &vertical, std::string &legend, cairo_iface *context) const;
    bool get_clear_all(int index) const;
    ~analyzer_audio_module();
    mutable int _mode_old;
    mutable bool _falling;
protected:
    static const int max_phase_buffer_size = 8192;
    int phase_buffer_size;
    float *phase_buffer;
    int fft_buffer_size;
    float *fft_buffer;
    int *spline_buffer;
    int plength;
    int ppos;
    int fpos;
    mutable fftwf_plan fft_plan;
    static const int max_fft_cache_size = 32768;
    static const int max_fft_buffer_size = max_fft_cache_size * 2;
    float *fft_inL, *fft_outL;
    float *fft_inR, *fft_outR;
    float *fft_smoothL, *fft_smoothR;
    float *fft_deltaL, *fft_deltaR;
    float *fft_holdL, *fft_holdR;
    float *fft_fallingL, *fft_fallingR;
    float *fft_freezeL, *fft_freezeR;
    mutable int lintrans;
    mutable int ____analyzer_phase_was_drawn_here;
    mutable int ____analyzer_sanitize;

};

};
#endif
