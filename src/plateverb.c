// src/plateverb.c
#include <lv2/core/lv2.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef LV2_SYMBOL_EXPORT
#define LV2_SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

#define PLATEVERB_URI "https://github.com/lilbrimstone/plateverb"

// ----- Utilities -----
static inline float clampf(float x, float lo, float hi) {
  return (x < lo) ? lo : (x > hi) ? hi : x;
}

static inline float maxf(float a, float b) {
  return (a > b) ? a : b;
}

// Fast Soft Clipper (tanh approximation)
// y = x * (27 + x*x) / (27 + 9*x*x) is a common fast approx, 
// but standard tanhf is usually optimized enough on ARM.
static inline float soft_clip(float x) {
    // Input gain boost happens before this function
    return tanhf(x);
}

// ----- One-pole lowpass -----
typedef struct {
  float z;
  float a; 
} OnePoleLP;

static inline void lp_init(OnePoleLP* lp, float a) {
  lp->z = 0.0f;
  lp->a = a;
}

static inline float lp_process(OnePoleLP* lp, float x) {
  const float y = (1.0f - lp->a) * x + lp->a * lp->z;
  lp->z = y;
  return y;
}

// ----- Circular Delay -----
typedef struct {
  float* buf;
  int size;
  int idx; 
} Delay;

static inline void delay_init(Delay* d, int size) {
  if (size < 8) size = 8;
  d->buf = (float*)calloc((size_t)size, sizeof(float));
  d->size = d->buf ? size : 0;
  d->idx = 0;
}

static inline void delay_free(Delay* d) {
  free(d->buf);
  d->buf = NULL;
  d->size = 0;
  d->idx = 0;
}

static inline float delay_read(const Delay* d, int tap) {
  int ri = d->idx - tap;
  while (ri < 0) ri += d->size;
  return d->buf[ri];
}

static inline float delay_read_linear(const Delay* d, float tap) {
  int32_t i_int = (int32_t)tap;
  float frac = tap - (float)i_int;
  int32_t r1 = d->idx - i_int;
  int32_t r2 = r1 - 1;
  while (r1 < 0) r1 += d->size; while (r1 >= d->size) r1 -= d->size;
  while (r2 < 0) r2 += d->size; while (r2 >= d->size) r2 -= d->size;
  float x1 = d->buf[r1];
  float x2 = d->buf[r2];
  return x1 + frac * (x2 - x1);
}

static inline void delay_write(Delay* d, float x) {
  d->buf[d->idx] = x;
  d->idx++;
  if (d->idx >= d->size) d->idx = 0;
}

// ----- Combs -----
typedef struct {
  Delay delay;
  OnePoleLP lp;    
  float feedback;  
  int   D;         
} Comb;

static inline void comb_init(Comb* c, int max_delay, int D_init, float fb, float lp_a) {
  delay_init(&c->delay, max_delay);
  lp_init(&c->lp, lp_a);
  c->feedback = fb;
  c->D = (D_init > 1) ? D_init : 1;
}

static inline float comb_process(Comb* c, float x, float fb_scale) {
  const float y = delay_read(&c->delay, c->D);
  const float damped = lp_process(&c->lp, y);
  delay_write(&c->delay, x + (c->feedback * fb_scale) * damped);
  return y;
}

static inline void comb_free(Comb* c) { delay_free(&c->delay); }

// ----- Allpass -----
typedef struct {
  Delay delay;
  float a; 
  int   D; 
} Allpass;

static inline void allpass_init(Allpass* ap, int max_delay, int D_init, float a) {
  delay_init(&ap->delay, max_delay);
  ap->a = a;
  ap->D = (D_init > 1) ? D_init : 1;
}

static inline float allpass_process(Allpass* ap, float x) {
  const float d = delay_read(&ap->delay, ap->D);
  const float y = d - ap->a * x;
  const float u = x + ap->a * y;
  delay_write(&ap->delay, u);
  return y;
}

static inline void allpass_free(Allpass* ap) { delay_free(&ap->delay); }

// ----- Reverb Core -----
#define NUM_COMBS        4
#define NUM_ALLPASSES    2
#define MAX_MS(ms, fs)   ((int)((ms) * 0.001f * (fs)) + 4)

typedef struct {
  // Ports
  const float* in;
  float* out_l;
  float* out_r;
  const float* p_mix;
  const float* p_predelay_ms;
  const float* p_decay_rt60;
  const float* p_damping;
  const float* p_diffusion;
  const float* p_size;
  const float* p_gate;
  const float* p_mod_depth;
  const float* p_mod_rate;
  const float* p_locut;
  // NEW PORT
  const float* p_grit;      // 0..1

  // State
  float sample_rate;
  float lfo_phase;
  float hp_in_z;
  float hp_out_z;

  Delay predelay; 

  Comb combL[NUM_COMBS];
  Comb combR[NUM_COMBS];
  Allpass apL[NUM_ALLPASSES];
  Allpass apR[NUM_ALLPASSES];

  int baseCombL[NUM_COMBS];
  int baseCombR[NUM_COMBS];
  int baseApL[NUM_ALLPASSES];
  int baseApR[NUM_ALLPASSES];

  int max_comb_len;
  int max_ap_len;
  int max_predelay_len;

  float gate_env;
  float gate_gain;
} PlateVerb;

static void set_default_base_delays(PlateVerb* self, float fs) {
  const float fs_ratio = fs > 1.0f ? (fs / 48000.0f) : 1.0f;
  const int combL_ref[NUM_COMBS] = { 1201, 1553, 1867, 2203 };
  const int combR_ref[NUM_COMBS] = { 1319, 1613, 1973, 2411 };
  const int apL_ref[NUM_ALLPASSES] = { 239, 421 };
  const int apR_ref[NUM_ALLPASSES] = { 263, 463 };

  for (int i = 0; i < NUM_COMBS; ++i) {
    int DL = (int)lrintf(combL_ref[i] * fs_ratio);
    int DR = (int)lrintf(combR_ref[i] * fs_ratio);
    self->baseCombL[i] = (DL < 16) ? 16 : DL;
    self->baseCombR[i] = (DR < 16) ? 16 : DR;
  }
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    int DL = (int)lrintf(apL_ref[i] * fs_ratio);
    int DR = (int)lrintf(apR_ref[i] * fs_ratio);
    self->baseApL[i] = (DL < 8) ? 8 : DL;
    self->baseApR[i] = (DR < 8) ? 8 : DR;
  }
}

static inline float comb_gain_from_rt60(float rt60, int D, float fs) {
  if (rt60 < 0.05f) rt60 = 0.05f;
  const float g = powf(10.0f, (-3.0f * (float)D) / (rt60 * fs));
  return clampf(g, 0.0f, 0.9999f);
}

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* p, const LV2_Feature* const* f) {
  (void)d; (void)p; (void)f;
  PlateVerb* self = (PlateVerb*)calloc(1, sizeof(PlateVerb));
  if (!self) return NULL;

  self->sample_rate = (float)(rate > 1.0 ? rate : 48000.0);
  
  set_default_base_delays(self, self->sample_rate);
  self->max_comb_len     = MAX_MS(80.0f, self->sample_rate);
  self->max_ap_len       = MAX_MS(50.0f, self->sample_rate); 
  self->max_predelay_len = MAX_MS(220.0f, self->sample_rate);

  delay_init(&self->predelay, self->max_predelay_len);

  for (int i = 0; i < NUM_COMBS; ++i) {
    comb_init(&self->combL[i], self->max_comb_len, self->baseCombL[i], 0.7f, 0.7f);
    comb_init(&self->combR[i], self->max_comb_len, self->baseCombR[i], 0.7f, 0.7f);
  }
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    allpass_init(&self->apL[i], self->max_ap_len, self->baseApL[i], 0.7f);
    allpass_init(&self->apR[i], self->max_ap_len, self->baseApR[i], 0.7f);
  }
  
  self->gate_gain = 1.0f;
  return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data_location) {
  PlateVerb* self = (PlateVerb*)instance;
  switch (port) {
    case 0: self->in            = (const float*)data_location; break;
    case 1: self->out_l         = (float*)data_location; break;
    case 2: self->out_r         = (float*)data_location; break;
    case 3: self->p_mix         = (const float*)data_location; break;
    case 4: self->p_predelay_ms = (const float*)data_location; break;
    case 5: self->p_decay_rt60  = (const float*)data_location; break;
    case 6: self->p_damping     = (const float*)data_location; break;
    case 7: self->p_diffusion   = (const float*)data_location; break;
    case 8: self->p_size        = (const float*)data_location; break;
    case 9: self->p_gate        = (const float*)data_location; break;
    case 10: self->p_mod_depth  = (const float*)data_location; break;
    case 11: self->p_mod_rate   = (const float*)data_location; break;
    case 12: self->p_locut      = (const float*)data_location; break;
    case 13: self->p_grit       = (const float*)data_location; break;
    default: break;
  }
}

static void activate(LV2_Handle instance) {
  PlateVerb* self = (PlateVerb*)instance;
  if (self->predelay.buf) { memset(self->predelay.buf, 0, (size_t)self->predelay.size * sizeof(float)); self->predelay.idx = 0; }
  for (int i = 0; i < NUM_COMBS; ++i) {
    if (self->combL[i].delay.buf) { memset(self->combL[i].delay.buf, 0, (size_t)self->combL[i].delay.size*4); self->combL[i].lp.z = 0; }
    if (self->combR[i].delay.buf) { memset(self->combR[i].delay.buf, 0, (size_t)self->combR[i].delay.size*4); self->combR[i].lp.z = 0; }
  }
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    if (self->apL[i].delay.buf) memset(self->apL[i].delay.buf, 0, (size_t)self->apL[i].delay.size*4);
    if (self->apR[i].delay.buf) memset(self->apR[i].delay.buf, 0, (size_t)self->apR[i].delay.size*4);
  }
  self->gate_env = 0.0f;
  self->gate_gain = 1.0f;
  self->lfo_phase = 0.0f;
  self->hp_in_z = 0.0f;
  self->hp_out_z = 0.0f;
}

static void run(LV2_Handle instance, uint32_t n_samples) {
  PlateVerb* self = (PlateVerb*)instance;

  const float* in  = self->in;
  float* outL = self->out_l;
  float* outR = self->out_r;

  // Controls
  const float mix     = self->p_mix         ? clampf(*self->p_mix,         0.0f, 1.0f)   : 0.25f;
  const float pre_ms  = self->p_predelay_ms ? clampf(*self->p_predelay_ms, 0.0f, 200.0f) : 20.0f;
  const float rt60    = self->p_decay_rt60  ? clampf(*self->p_decay_rt60,  0.1f, 20.0f)  : 2.5f;
  const float damp    = self->p_damping     ? clampf(*self->p_damping,     0.0f, 1.0f)   : 0.5f;
  const float diff    = self->p_diffusion   ? clampf(*self->p_diffusion,   0.0f, 1.0f)   : 0.7f;
  const float sizeK   = self->p_size        ? clampf(*self->p_size,        0.5f, 1.5f)   : 1.0f;
  const float gateKnob= self->p_gate        ? clampf(*self->p_gate,        0.0f, 1.0f)   : 0.0f;
  const float modDepth= self->p_mod_depth   ? clampf(*self->p_mod_depth,   0.0f, 5.0f)   : 1.0f;
  const float modRate = self->p_mod_rate    ? clampf(*self->p_mod_rate,    0.0f, 5.0f)   : 0.5f;
  const float hp_freq = self->p_locut       ? clampf(*self->p_locut,       10.0f, 1000.0f) : 10.0f;
  const float grit    = self->p_grit        ? clampf(*self->p_grit,        0.0f, 1.0f)   : 0.0f;

  const float dt = 1.0f / self->sample_rate;
  const float rc_hp = 1.0f / (6.2831853f * hp_freq); 
  const float hp_alpha = rc_hp / (rc_hp + dt);

  int pred_samp = (int)lrintf(pre_ms * 0.001f * self->sample_rate);
  if (pred_samp >= self->predelay.size) pred_samp = self->predelay.size - 1;

  // Grit Pre-calculation: 1.0 (clean) to 12.0 (heavily boosted)
  const float drive_gain = 1.0f + (grit * 11.0f);

  const float ap_a = 0.3f + 0.55f * diff;
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    self->apL[i].a = ap_a; self->apR[i].a = ap_a;
    int DL = (int)lrintf((float)self->baseApL[i] * sizeK);
    int DR = (int)lrintf((float)self->baseApR[i] * sizeK);
    if (DL >= self->apL[i].delay.size - 250) DL = self->apL[i].delay.size - 250;
    if (DR >= self->apR[i].delay.size - 250) DR = self->apR[i].delay.size - 250;
    self->apL[i].D = DL; self->apR[i].D = DR;
  }
  const float lp_a = 0.5f + 0.48f * damp;
  for (int i = 0; i < NUM_COMBS; ++i) {
    int DL = (int)lrintf((float)self->baseCombL[i] * sizeK);
    int DR = (int)lrintf((float)self->baseCombR[i] * sizeK);
    if (DL >= self->combL[i].delay.size) DL = self->combL[i].delay.size - 1;
    if (DR >= self->combR[i].delay.size) DR = self->combR[i].delay.size - 1;
    self->combL[i].D = DL; self->combR[i].D = DR;
    self->combL[i].feedback = comb_gain_from_rt60(rt60, DL, self->sample_rate);
    self->combR[i].feedback = comb_gain_from_rt60(rt60, DR, self->sample_rate);
    self->combL[i].lp.a = lp_a; self->combR[i].lp.a = lp_a;
  }

  // Gate Constants
  const int gate_enabled = (gateKnob > 0.0001f) ? 1 : 0;
  const float gate_dB = -60.0f + 60.0f * gateKnob;
  const float gate_thr = gate_enabled ? powf(10.0f, gate_dB / 20.0f) : 0.0f;
  const float ea = expf(-1.0f / (self->sample_rate * 0.003f));
  const float er = expf(-1.0f / (self->sample_rate * 0.050f));
  const float ga = expf(-1.0f / (self->sample_rate * 0.002f));
  const float gr = expf(-1.0f / (self->sample_rate * 0.020f));

  const float lfo_inc = (modRate * 6.2831853f) / self->sample_rate;
  const float mod_samp = modDepth * 0.001f * self->sample_rate;

  for (uint32_t n = 0; n < n_samples; ++n) {
    const float x = in ? in[n] : 0.0f;

    // 1. Predelay
    delay_write(&self->predelay, x); 
    float predWet = delay_read(&self->predelay, pred_samp + 1);

    // 2. High Pass Filter
    float hp_out = hp_alpha * (self->hp_out_z + predWet - self->hp_in_z);
    self->hp_in_z = predWet;
    self->hp_out_z = hp_out;
    predWet = hp_out;

    // 3. NEW: Grit (Input Saturation)
    // Apply boost and soft clip *before* filling the tank
    if (grit > 0.001f) {
        predWet = soft_clip(predWet * drive_gain);
    }

    // 4. Combs
    float fb_modifier = gate_enabled ? self->gate_gain : 1.0f;
    float sL = 0.0f, sR = 0.0f;
    for (int i = 0; i < NUM_COMBS; ++i) {
      sL += comb_process(&self->combL[i], predWet, fb_modifier);
      sR += comb_process(&self->combR[i], predWet, fb_modifier);
    }
    sL *= 0.25f; sR *= 0.25f;

    // 5. Modulated Allpass
    self->lfo_phase += lfo_inc;
    if (self->lfo_phase > 6.2831853f) self->lfo_phase -= 6.2831853f;
    const float lfo_sin = sinf(self->lfo_phase);
    const float lfo_cos = cosf(self->lfo_phase);

    float yL = sL, yR = sR;
    for (int i = 0; i < NUM_ALLPASSES; ++i) {
        float pol = (i % 2 == 0) ? 1.0f : -1.0f;
        float dL_mod = (float)self->apL[i].D + (lfo_sin * mod_samp * pol);
        float dR_mod = (float)self->apR[i].D + (lfo_cos * mod_samp * pol);
        
        if (dL_mod < 4.0f) dL_mod = 4.0f; if (dR_mod < 4.0f) dR_mod = 4.0f;
        if (dL_mod > (float)self->apL[i].delay.size - 4.0f) dL_mod = (float)self->apL[i].delay.size - 4.0f;
        if (dR_mod > (float)self->apR[i].delay.size - 4.0f) dR_mod = (float)self->apR[i].delay.size - 4.0f;
        
        float delayedL = delay_read_linear(&self->apL[i].delay, dL_mod);
        float outL_ap = delayedL - self->apL[i].a * yL;
        float inL_ap  = yL + self->apL[i].a * outL_ap;
        delay_write(&self->apL[i].delay, inL_ap);
        yL = outL_ap;

        float delayedR = delay_read_linear(&self->apR[i].delay, dR_mod);
        float outR_ap = delayedR - self->apR[i].a * yR;
        float inR_ap  = yR + self->apR[i].a * outR_ap;
        delay_write(&self->apR[i].delay, inR_ap);
        yR = outR_ap;
    }

    // 6. Gate (Stereo Linked)
    if (gate_enabled) {
      const float trigger = maxf(fabsf(yL), fabsf(yR));
      self->gate_env = (trigger > self->gate_env) 
                     ? (ea * self->gate_env + (1.0f - ea) * trigger) 
                     : (er * self->gate_env + (1.0f - er) * trigger);
      const float target = (self->gate_env >= gate_thr) ? 1.0f 
                         : (self->gate_env <= gate_thr * 0.7f) ? 0.0f 
                         : self->gate_gain;
      self->gate_gain = (target > self->gate_gain) 
                      ? (ga * self->gate_gain + (1.0f - ga) * target) 
                      : (gr * self->gate_gain + (1.0f - gr) * target);
      yL *= self->gate_gain;
      yR *= self->gate_gain;
    }

    outL[n] = (1.0f - mix) * x + mix * yL;
    outR[n] = (1.0f - mix) * x + mix * yR;
  }
}

static void deactivate(LV2_Handle instance) { (void)instance; }
static void cleanup(LV2_Handle instance) {
  PlateVerb* self = (PlateVerb*)instance;
  // Cleanup logic same as before...
  delay_free(&self->predelay);
  for (int i = 0; i < NUM_COMBS; ++i) { comb_free(&self->combL[i]); comb_free(&self->combR[i]); }
  for (int i = 0; i < NUM_ALLPASSES; ++i) { allpass_free(&self->apL[i]); allpass_free(&self->apR[i]); }
  free(self);
}
static const void* extension_data(const char* uri) { (void)uri; return NULL; }
static const LV2_Descriptor descriptor = {
  PLATEVERB_URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data
};
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  switch (index) { case 0: return &descriptor; default: return NULL; }
}