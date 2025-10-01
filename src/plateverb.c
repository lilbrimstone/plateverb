// src/plateverb.c
#include <lv2/core/lv2.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef LV2_SYMBOL_EXPORT
#define LV2_SYMBOL_EXPORT __attribute__((visibility("default")))
#endif

// Must match manifest.ttl and plateverb.ttl
#define PLATEVERB_URI "https://github.com/lilbrimstone/plateverb"

// ----- Utilities -----
static inline float clampf(float x, float lo, float hi) {
  return (x < lo) ? lo : (x > hi) ? hi : x;
}

// ----- One-pole lowpass (feedback damping) -----
typedef struct {
  float z;
  float a; // [0..1], higher -> darker/smoother
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

// ----- Circular delay line (integer indexing) -----
typedef struct {
  float* buf;
  int size;
  int idx; // write index
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

static inline void delay_write(Delay* d, float x) {
  d->buf[d->idx] = x;
  d->idx++;
  if (d->idx >= d->size) d->idx = 0;
}

// ----- Schroeder comb (with damping in feedback) -----
typedef struct {
  Delay delay;
  OnePoleLP lp;    // damping in feedback
  float feedback;  // loop gain
  int   D;         // samples
} Comb;

static inline void comb_init(Comb* c, int max_delay, int D_init, float fb, float lp_a) {
  delay_init(&c->delay, max_delay);
  lp_init(&c->lp, lp_a);
  c->feedback = fb;
  c->D = (D_init > 1) ? D_init : 1;
}

static inline float comb_process(Comb* c, float x) {
  const float y = delay_read(&c->delay, c->D);
  const float damped = lp_process(&c->lp, y);
  delay_write(&c->delay, x + c->feedback * damped);
  return y;
}

static inline void comb_free(Comb* c) {
  delay_free(&c->delay);
}

// ----- Allpass diffuser -----
typedef struct {
  Delay delay;
  float a; // ~0.3..0.85
  int   D; // samples
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

static inline void allpass_free(Allpass* ap) {
  delay_free(&ap->delay);
}

// ----- Reverb core -----
#define NUM_COMBS        4
#define NUM_ALLPASSES    2
#define MAX_MS(ms, fs)   ((int)((ms) * 0.001f * (fs)) + 4)

typedef struct {
  // Audio ports
  const float* in;
  float* out_l;
  float* out_r;

  // Control ports
  const float* p_mix;         // 0..1
  const float* p_predelay_ms; // 0..200 (WET ONLY)
  const float* p_decay_rt60;  // 0.1..20
  const float* p_damping;     // 0..1
  const float* p_diffusion;   // 0..1
  const float* p_size;        // 0.5..1.5
  const float* p_gate;        // 0..1 (0 disables; >0 maps -60..0 dB threshold)

  // State
  float sample_rate;

  Delay predelay; // feeds wet path only

  // Stereo reverb cores
  Comb combL[NUM_COMBS];
  Comb combR[NUM_COMBS];
  Allpass apL[NUM_ALLPASSES];
  Allpass apR[NUM_ALLPASSES];

  // Base delays (scaled from 48k)
  int baseCombL[NUM_COMBS];
  int baseCombR[NUM_COMBS];
  int baseApL[NUM_ALLPASSES];
  int baseApR[NUM_ALLPASSES];

  // Max allocated sizes
  int max_comb_len;
  int max_ap_len;
  int max_predelay_len;

  // Gate state
  float gate_envL;
  float gate_envR;
  float gate_gainL;
  float gate_gainR;
} PlateVerb;

static void set_default_base_delays(PlateVerb* self, float fs) {
  const float fs_ratio = fs > 1.0f ? (fs / 48000.0f) : 1.0f;

  // Mutually prime-ish to reduce modal overlap
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

// RT60 mapping: y[n] = x[n] + g y[n-D], energy -60 dB at rt60 seconds
static inline float comb_gain_from_rt60(float rt60, int D, float fs) {
  if (rt60 < 0.05f) rt60 = 0.05f;
  const float g = powf(10.0f, (-3.0f * (float)D) / (rt60 * fs));
  return clampf(g, 0.0f, 0.9999f);
}

static LV2_Handle instantiate(
  const LV2_Descriptor*     descriptor,
  double                    rate,
  const char*               bundle_path,
  const LV2_Feature* const* features)
{
  (void)descriptor; (void)bundle_path; (void)features;
  PlateVerb* self = (PlateVerb*)calloc(1, sizeof(PlateVerb));
  if (!self) return NULL;

  self->sample_rate = (float)(rate > 1.0 ? rate : 48000.0);

  set_default_base_delays(self, self->sample_rate);
  self->max_comb_len     = MAX_MS(80.0f, self->sample_rate);   // ~80 ms
  self->max_ap_len       = MAX_MS(20.0f, self->sample_rate);   // ~20 ms
  self->max_predelay_len = MAX_MS(220.0f, self->sample_rate);  // up to 200 ms

  delay_init(&self->predelay, self->max_predelay_len);

  for (int i = 0; i < NUM_COMBS; ++i) {
    comb_init(&self->combL[i], self->max_comb_len, self->baseCombL[i], 0.7f, 0.7f);
    comb_init(&self->combR[i], self->max_comb_len, self->baseCombR[i], 0.7f, 0.7f);
  }
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    allpass_init(&self->apL[i], self->max_ap_len, self->baseApL[i], 0.7f);
    allpass_init(&self->apR[i], self->max_ap_len, self->baseApR[i], 0.7f);
  }

  // Gate init
  self->gate_envL = 0.0f;
  self->gate_envR = 0.0f;
  self->gate_gainL = 1.0f;
  self->gate_gainR = 1.0f;

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
    default: break;
  }
}

static void activate(LV2_Handle instance) {
  PlateVerb* self = (PlateVerb*)instance;

  if (self->predelay.buf) {
    memset(self->predelay.buf, 0, (size_t)self->predelay.size * sizeof(float));
    self->predelay.idx = 0;
  }
  for (int i = 0; i < NUM_COMBS; ++i) {
    if (self->combL[i].delay.buf) {
      memset(self->combL[i].delay.buf, 0, (size_t)self->combL[i].delay.size * sizeof(float));
      self->combL[i].delay.idx = 0;
      self->combL[i].lp.z = 0.0f;
    }
    if (self->combR[i].delay.buf) {
      memset(self->combR[i].delay.buf, 0, (size_t)self->combR[i].delay.size * sizeof(float));
      self->combR[i].delay.idx = 0;
      self->combR[i].lp.z = 0.0f;
    }
  }
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    if (self->apL[i].delay.buf) { memset(self->apL[i].delay.buf, 0, (size_t)self->apL[i].delay.size * sizeof(float)); self->apL[i].delay.idx = 0; }
    if (self->apR[i].delay.buf) { memset(self->apR[i].delay.buf, 0, (size_t)self->apR[i].delay.size * sizeof(float)); self->apR[i].delay.idx = 0; }
  }

  // Reset gate state
  self->gate_envL = self->gate_envR = 0.0f;
  self->gate_gainL = self->gate_gainR = 1.0f;
}

static void run(LV2_Handle instance, uint32_t n_samples) {
  PlateVerb* self = (PlateVerb*)instance;

  const float* in  = self->in;
  float* outL = self->out_l;
  float* outR = self->out_r;

  // Controls (clamped)
  const float mix     = self->p_mix         ? clampf(*self->p_mix,         0.0f, 1.0f)   : 0.25f;
  const float pre_ms  = self->p_predelay_ms ? clampf(*self->p_predelay_ms, 0.0f, 200.0f) : 20.0f;
  const float rt60    = self->p_decay_rt60  ? clampf(*self->p_decay_rt60,  0.1f, 20.0f)  : 2.5f;
  const float damp    = self->p_damping     ? clampf(*self->p_damping,     0.0f, 1.0f)   : 0.5f;
  const float diff    = self->p_diffusion   ? clampf(*self->p_diffusion,   0.0f, 1.0f)   : 0.7f;
  const float sizeK   = self->p_size        ? clampf(*self->p_size,        0.5f, 1.5f)   : 1.0f;
  const float gateKnob= self->p_gate        ? clampf(*self->p_gate,        0.0f, 1.0f)   : 0.0f;

  // Predelay samples for WET ONLY
  int pred_samp = (int)lrintf(pre_ms * 0.001f * self->sample_rate);
  if (pred_samp < 0) pred_samp = 0;
  if (pred_samp >= self->predelay.size) pred_samp = self->predelay.size - 1;

  // Diffusion: map knob to allpass 'a' in [0.3..0.85]
  const float ap_a = 0.3f + 0.55f * diff;
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    self->apL[i].a = ap_a;
    self->apR[i].a = ap_a;

    int DL = (int)lrintf((float)self->baseApL[i] * sizeK);
    int DR = (int)lrintf((float)self->baseApR[i] * sizeK);
    if (DL < 8) DL = 8; if (DR < 8) DR = 8;
    if (DL >= self->apL[i].delay.size) DL = self->apL[i].delay.size - 1;
    if (DR >= self->apR[i].delay.size) DR = self->apR[i].delay.size - 1;
    self->apL[i].D = DL;
    self->apR[i].D = DR;
  }

  // Damping: map knob to LP coefficient a in [0.5..0.98]
  const float lp_a = 0.5f + 0.48f * damp;

  // Update comb delays and feedback per block
  for (int i = 0; i < NUM_COMBS; ++i) {
    int DL = (int)lrintf((float)self->baseCombL[i] * sizeK);
    int DR = (int)lrintf((float)self->baseCombR[i] * sizeK);
    if (DL < 16) DL = 16; if (DR < 16) DR = 16;
    if (DL >= self->combL[i].delay.size) DL = self->combL[i].delay.size - 1;
    if (DR >= self->combR[i].delay.size) DR = self->combR[i].delay.size - 1;
    self->combL[i].D = DL; self->combR[i].D = DR;

    self->combL[i].feedback = comb_gain_from_rt60(rt60, DL, self->sample_rate);
    self->combR[i].feedback = comb_gain_from_rt60(rt60, DR, self->sample_rate);

    self->combL[i].lp.a = lp_a;
    self->combR[i].lp.a = lp_a;
  }

  // Gate configuration
  // Gate knob: 0 disables. >0 maps linearly from -60 dB .. 0 dB threshold.
  const int gate_enabled = (gateKnob > 0.0001f) ? 1 : 0;
  const float gate_dB = -60.0f + 60.0f * gateKnob;                  // [-60, 0] dB
  const float gate_thr = gate_enabled ? powf(10.0f, gate_dB / 20.0f) : 0.0f; // linear
  // Envelope follower attack/release and gain smoothing (in seconds)
  const float env_attack_s  = 0.003f;  // ~3 ms
  const float env_release_s = 0.050f;  // ~50 ms
  const float gain_attack_s = 0.002f;  // gate opening ~2 ms
  const float gain_release_s= 0.020f;  // gate closing ~20 ms
  const float ea = expf(-1.0f / (self->sample_rate * env_attack_s));
  const float er = expf(-1.0f / (self->sample_rate * env_release_s));
  const float ga = expf(-1.0f / (self->sample_rate * gain_attack_s));
  const float gr = expf(-1.0f / (self->sample_rate * gain_release_s));

  for (uint32_t n = 0; n < n_samples; ++n) {
    const float x = in ? in[n] : 0.0f;

    // Wet predelay only
    const float predWet = delay_read(&self->predelay, pred_samp);
    delay_write(&self->predelay, x);

    // Parallel combs fed by predelayed signal (wet path)
    float sL = 0.0f, sR = 0.0f;
    for (int i = 0; i < NUM_COMBS; ++i) {
      sL += comb_process(&self->combL[i], predWet);
      sR += comb_process(&self->combR[i], predWet);
    }
    sL *= 1.0f / (float)NUM_COMBS;
    sR *= 1.0f / (float)NUM_COMBS;

    // Allpass diffusion (wet path)
    float yL = sL, yR = sR;
    for (int i = 0; i < NUM_ALLPASSES; ++i) {
      yL = allpass_process(&self->apL[i], yL);
      yR = allpass_process(&self->apR[i], yR);
    }

    // Gate on wet only
    float wetL = yL, wetR = yR;
    if (gate_enabled) {
      const float inAbsL = fabsf(wetL);
      const float inAbsR = fabsf(wetR);

      self->gate_envL = (inAbsL > self->gate_envL)
                          ? (ea * self->gate_envL + (1.0f - ea) * inAbsL)
                          : (er * self->gate_envL + (1.0f - er) * inAbsL);
      self->gate_envR = (inAbsR > self->gate_envR)
                          ? (ea * self->gate_envR + (1.0f - ea) * inAbsR)
                          : (er * self->gate_envR + (1.0f - er) * inAbsR);

      const float openThr  = gate_thr;
      const float closeThr = gate_thr * 0.7f; // small hysteresis (~3 dB)

      const float targetL = (self->gate_envL >= openThr)  ? 1.0f
                          : (self->gate_envL <= closeThr) ? 0.0f
                          : self->gate_gainL;
      const float targetR = (self->gate_envR >= openThr)  ? 1.0f
                          : (self->gate_envR <= closeThr) ? 0.0f
                          : self->gate_gainR;

      self->gate_gainL = (targetL > self->gate_gainL)
                           ? (ga * self->gate_gainL + (1.0f - ga) * targetL)
                           : (gr * self->gate_gainL + (1.0f - gr) * targetL);
      self->gate_gainR = (targetR > self->gate_gainR)
                           ? (ga * self->gate_gainR + (1.0f - ga) * targetR)
                           : (gr * self->gate_gainR + (1.0f - gr) * targetR);

      wetL *= self->gate_gainL;
      wetR *= self->gate_gainR;
    }

    // Dry/Wet: dry = direct input (no predelay), wet = predelayed tail
    const float dry = x;
    outL[n] = (1.0f - mix) * dry + mix * wetL;
    outR[n] = (1.0f - mix) * dry + mix * wetR;
  }
}

static void deactivate(LV2_Handle instance) { (void)instance; }

static void cleanup(LV2_Handle instance) {
  PlateVerb* self = (PlateVerb*)instance;

  delay_free(&self->predelay);
  for (int i = 0; i < NUM_COMBS; ++i) {
    comb_free(&self->combL[i]);
    comb_free(&self->combR[i]);
  }
  for (int i = 0; i < NUM_ALLPASSES; ++i) {
    allpass_free(&self->apL[i]);
    allpass_free(&self->apR[i]);
  }

  free(self);
}

static const void* extension_data(const char* uri) {
  (void)uri;
  return NULL;
}

static const LV2_Descriptor descriptor = {
  PLATEVERB_URI,
  instantiate,
  connect_port,
  activate,
  run,
  deactivate,
  cleanup,
  extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  switch (index) {
    case 0: return &descriptor;
    default: return NULL;
  }
}