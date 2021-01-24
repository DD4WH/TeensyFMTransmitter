/*
   Legal note

  TeensyFMTransmitter is an experimental program, designed only for experimentation.
  It is in no way intended to become a personal media center or a tool to operate a radio station,
  or even broadcast sound to one's own stereo system.

  In most countries, transmitting radio waves without a state-issued licence specific
  to the transmission modalities (frequency, power, bandwidth, etc.) is illegal.

  Therefore, always connect a shielded transmission line from the Teensy directly to a radio receiver,
  so as not to emit radio waves. Never use an antenna.

  Even if you are a licensed amateur radio operator, using TeensyFMTransmitter to transmit
  radio waves on ham frequencies without any filtering between the Teensy and an antenna is most probably illegal
  because the square-wave carrier is very rich in harmonics, so the bandwidth requirements are likely not met.

  I could not be held liable for any misuse of your own Teensy.
  Any experiment is made under your own responsibility.

*/

//  (c) Frank B

#if defined(__IMXRT1062__)

#include "output_fm.h"

#define INTERPOLATION 4

#if !defined(INTERPOLATION) || INTERPOLATION < 1
#error Set INTERPOLATION > 0 !
#endif

#define NUM_SAMPLES (AUDIO_BLOCK_SAMPLES / 2) //Input samples
#define I_NUM_SAMPLES (NUM_SAMPLES * INTERPOLATION)

#define PREEMPHASIS_DISABLE 0 //Testing

#define OCRAM32 DMAMEM static  __attribute__((aligned(32)))

OCRAM32 int fm_tx_buffer[I_NUM_SAMPLES * 2];
audio_block_t * AudioOutputFM::block_left = NULL;
audio_block_t * AudioOutputFM::block_right = NULL;
unsigned long AudioOutputFM::us = 0;
bool AudioOutputFM::mono;
bool AudioOutputFM::update_responsibility = false;
DMAChannel AudioOutputFM::dma(false);


static double FM_MHz;
static const double FM_deviation = 75000.0 * 4.0;
static double FS; //PLL Frequency
static const unsigned ndiv = 0xffffff;


typedef float fdata_t[I_NUM_SAMPLES];



#if INTERPOLATION > 1
static const unsigned interpolation_taps = 32;
static const float n_att = 90.0; // desired stopband attenuation
static const float interpol_cutoff = AUDIO_SAMPLE_RATE_EXACT / 2.0f; // 
OCRAM32 float interpolation_coeffs[interpolation_taps];
OCRAM32 float interpolation_L_state[NUM_SAMPLES + interpolation_taps / INTERPOLATION];
OCRAM32 float interpolation_R_state[NUM_SAMPLES + interpolation_taps / INTERPOLATION];
static arm_fir_interpolate_instance_f32 interpolationL;
static arm_fir_interpolate_instance_f32 interpolationR;
#endif


//preemphasis:
static float pre_a0;
static float pre_a1;
static float pre_b;


//forward declarations:
extern "C" void xbar_connect(unsigned int input, unsigned int output);
extern void calc_FIR_coeffs (float * coeffs_I, int numCoeffs, float fc, float Astop, int type, float dfc, float Fsamprate);
static void process(audio_block_t *blockL, audio_block_t *blockR, unsigned offset);
static void processMono(fdata_t blockL, fdata_t blockR, const unsigned offset);
static void processStereo(fdata_t blockL, fdata_t blockR, const unsigned offset);


// https://www.nxp.com/docs/en/application-note/AN5078.pdf
#define PADCONFIG ((0 << 0) | (1 << 3) | (1 << 6)) //Pg 622 Reference Manual
/*
        (1 << 0) | // Slew rate (1 = fast)
        (1 << 3) | // 3 Bit Driver Strength;
        (1 << 6);  // 2 Speed
*/

FLASHMEM
void AudioOutputFM::begin(uint8_t mclk_pin, unsigned MHz, int preemphasis)
{
  memset(fm_tx_buffer, 0, sizeof(fm_tx_buffer));

  dma.begin(true); // Allocate the DMA channel first

  mono = true;

  FM_MHz = MHz;
  FS = FM_MHz * 1000000 * 4;


  // pre-emphasis has been based on the filter designed by Jonti
  // use more sophisticated pre-emphasis filter: https://jontio.zapto.org/download/preempiir.pdf
  // https://github.com/jontio/JMPX/blob/master/libJMPX/JDSP.cpp
  // TODO: uncomment this in case that a sample rate of 192ksps is used !
  // sample rate 192ksps
  
  // ATTENTION: SampleRate 44.1 * 4 = 176kHz
#if INTERPOLATION > 1 
  
    if(preemphasis == PREEMPHASIS_50)
    {
      pre_a0 = 5.309858008;
      pre_a1 = -4.794606188;
      pre_b = 0.4847481783;
    }
    else
    {
      pre_a0 = 7.681633687l;
      pre_a1 = -7.170926091l;
      pre_b = 0.4892924010l;
    }
#else
  //ATTENTION: 
  // this is a more sophisticated pre-emphasis filter: https://jontio.zapto.org/download/preempiir.pdf
  // filter coefficients calculated for new sample rate of 44.1ksps by DD4WH, 2021-01-23
  // sample rate 44.1ksps
  if (preemphasis == PREEMPHASIS_50)
  {
    pre_a0 = 4.655206052723760;
    pre_a1 = -2.911399421812300;
    pre_b = -0.743806630911458;
  }
  else
  { // PREAMPHASIS_75
    pre_a0 = 6.597864306804010;
    pre_a1 = -4.854202108640480;
    pre_b = -0.743662198163528;
  }
#endif


#if INTERPOLATION > 1
   memset(interpolation_coeffs, 0, sizeof(interpolation_coeffs));
   memset(interpolation_L_state, 0, sizeof(interpolation_L_state));
   memset(interpolation_R_state, 0, sizeof(interpolation_R_state));
  
  //calc_FIR_coeffs (float * coeffs_I, int numCoeffs, float fc, float Astop, int type, float dfc, float Fsamprate)
  // the interpolation filter is AFTER the upsampling, so it has to be in the target sample rate
  calc_FIR_coeffs(interpolation_coeffs, interpolation_taps, interpol_cutoff, n_att, 0, 0.0, AUDIO_SAMPLE_RATE_EXACT * INTERPOLATION);

  /*
     arm_status arm_fir_interpolate_init_f32  (   
      arm_fir_interpolate_instance_f32 *    S,
      uint8_t   L,
      uint16_t    numTaps,
      const float32_t *   pCoeffs,
      float32_t *   pState,
      uint32_t    blockSize
    )
  */
   // I think you should initiate the number of input samples, NOT the number of interpolated samples
  if (arm_fir_interpolate_init_f32(&interpolationL, INTERPOLATION, interpolation_taps, interpolation_coeffs, interpolation_L_state, NUM_SAMPLES) ||
      arm_fir_interpolate_init_f32(&interpolationR, INTERPOLATION, interpolation_taps, interpolation_coeffs, interpolation_R_state, NUM_SAMPLES))
  {
    Serial.println("Init of interpolation failed");
    while (1);
  }

#endif
  
  int n1 = 2; //SAI prescaler
  int n2 = 1 + (24000000.0 * 27) / (FS * n1);
  double C = (FS * n1 * n2) / 24000000.0;
  int nfact = C;
  int nmult = C * ndiv - (nfact * ndiv);
   
  CCM_ANALOG_PLL_VIDEO = CCM_ANALOG_PLL_VIDEO_BYPASS | CCM_ANALOG_PLL_VIDEO_ENABLE
                         | CCM_ANALOG_PLL_VIDEO_POST_DIV_SELECT(0) // 0: 1/4; 1: 1/2; 2: 1/1
                         | CCM_ANALOG_PLL_VIDEO_DIV_SELECT(nfact);

  CCM_ANALOG_PLL_VIDEO_NUM   = nmult;
  CCM_ANALOG_PLL_VIDEO_DENOM = ndiv;

  CCM_ANALOG_PLL_VIDEO &= ~CCM_ANALOG_PLL_VIDEO_POWERDOWN;//Switch on PLL
  while (!(CCM_ANALOG_PLL_VIDEO & CCM_ANALOG_PLL_VIDEO_LOCK)) {}; //Wait for pll-lock

  CCM_ANALOG_PLL_VIDEO &= ~CCM_ANALOG_PLL_VIDEO_BYPASS;//Disable Bypass

  //ENABLE I2S
  if (mclk_pin == 33) {
    CCM_CCGR5 |= CCM_CCGR5_SAI2(CCM_CCGR_ON);
    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI2_CLK_SEL_MASK))
                 | CCM_CSCMR1_SAI2_CLK_SEL(1); // &0x03 // (0,1,2): PLL3PFD0, PLL5, PLL4,
    CCM_CS2CDR = (CCM_CS2CDR & ~(CCM_CS2CDR_SAI2_CLK_PRED_MASK | CCM_CS2CDR_SAI2_CLK_PODF_MASK))
                 | CCM_CS2CDR_SAI2_CLK_PRED(n1 - 1)
                 | CCM_CS2CDR_SAI2_CLK_PODF(n2 - 1);
    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI2_MCLK3_SEL_MASK))
                      | (IOMUXC_GPR_GPR1_SAI2_MCLK_DIR | IOMUXC_GPR_GPR1_SAI2_MCLK3_SEL(0));  //Select MCLK

    I2S2_TMR = 0;
    I2S2_TCR2 = I2S_TCR2_MSEL(1);
    CORE_PIN33_CONFIG = 2;  // EMC_07, 2=SAI2_MCLK
    CORE_PIN33_PADCONFIG = PADCONFIG;
  }

  else if (mclk_pin == 30) {
    CCM_CCGR5 |= CCM_CCGR5_SAI3(CCM_CCGR_ON);
    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI3_CLK_SEL_MASK))
                 | CCM_CSCMR1_SAI3_CLK_SEL(1); // &0x03 // (0,1,2): PLL3PFD0, PLL5, PLL4,
    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI3_CLK_PRED_MASK | CCM_CS1CDR_SAI3_CLK_PODF_MASK))
                 | CCM_CS1CDR_SAI3_CLK_PRED(n1 - 1)
                 | CCM_CS1CDR_SAI3_CLK_PODF(n2 - 1);
    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI3_MCLK3_SEL_MASK))
                      | (IOMUXC_GPR_GPR1_SAI3_MCLK_DIR | IOMUXC_GPR_GPR1_SAI3_MCLK3_SEL(0));  //Select MCLK

    I2S3_TMR = 0;
    I2S3_TCR2 = I2S_TCR2_MSEL(1);
    CORE_PIN30_CONFIG = 3; // EMC_37, 3=SAI3_MCLK
    CORE_PIN30_PADCONFIG = PADCONFIG;
  }

  else if (mclk_pin == 23) {
    CCM_CCGR5 |= CCM_CCGR5_SAI1(CCM_CCGR_ON);
    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI1_CLK_SEL_MASK))
                 | CCM_CSCMR1_SAI1_CLK_SEL(1); // &0x03 // (0,1,2): PLL3PFD0, PLL5, PLL4
    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK))
                 | CCM_CS1CDR_SAI1_CLK_PRED(n1 - 1) // &0x07
                 | CCM_CS1CDR_SAI1_CLK_PODF(n2 - 1); // &0x3f

    // Select MCLK
    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL_MASK))
                      | (IOMUXC_GPR_GPR1_SAI1_MCLK_DIR | IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL(0));

    I2S1_TMR = 0;
    I2S1_TCR2 = I2S_TCR2_MSEL(1);

    CORE_PIN23_CONFIG = 3;
    CORE_PIN23_PADCONFIG = PADCONFIG;

  }

  //QTimer
  const int comp1 = ((float)F_BUS_ACTUAL) / (INTERPOLATION * AUDIO_SAMPLE_RATE_EXACT / 2.0f) / 2.0f + 0.5f;
  TMR4_ENBL &= ~(1 << 3); //Disable
  TMR4_SCTRL3 = TMR_SCTRL_OEN | TMR_SCTRL_FORCE;
  TMR4_CSCTRL3 = TMR_CSCTRL_CL1(1) /*| TMR_CSCTRL_TCF1EN*/;
  TMR4_CNTR3 = 0;
  TMR4_LOAD3 = 0;
  TMR4_COMP13 = comp1;
  TMR4_CMPLD13 = comp1;
  TMR4_CTRL3 = TMR_CTRL_CM(1) | TMR_CTRL_PCS(8) | TMR_CTRL_LENGTH | TMR_CTRL_OUTMODE(3);
  /*Comparator Preload Register 1 DMA Enable
    Setting this bit enables DMA write requests for CMPLD1 whenever data is transferred out of the CMPLD1
    register into the COMP1 register.
  */
  TMR4_DMA3 = TMR_DMA_CMPLD1DE;
  TMR4_CNTR3 = 0;
  TMR4_ENBL |= (1 << 3); //Enable

  //route the timer output through XBAR to edge trigger DMA request
  CCM_CCGR2 |= CCM_CCGR2_XBAR1(CCM_CCGR_ON);  //enable XBAR
  xbar_connect(XBARA1_IN_QTIMER4_TIMER3, XBARA1_OUT_DMA_CH_MUX_REQ30);
  XBARA1_CTRL0 = XBARA_CTRL_STS0 | XBARA_CTRL_EDGE0(3) | XBARA_CTRL_DEN0;

  //setup DMA
  dma.TCD->SADDR = fm_tx_buffer;
  dma.TCD->SOFF = 4;
  dma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
  dma.TCD->NBYTES_MLNO = 4;
  dma.TCD->SLAST = -sizeof(fm_tx_buffer);
  dma.TCD->DOFF = 0;
  dma.TCD->CITER_ELINKNO = sizeof(fm_tx_buffer) / 4;
  dma.TCD->DLASTSGA = 0;
  dma.TCD->BITER_ELINKNO = sizeof(fm_tx_buffer) / 4;
  dma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
  dma.TCD->DADDR = (void *)((uint32_t)&CCM_ANALOG_PLL_VIDEO_NUM);

  dma.triggerAtHardwareEvent(DMAMUX_SOURCE_XBAR1_0);
  dma.attachInterrupt(dmaISR);

  dma.enable();

  update_responsibility = update_setup();

}

//Update Audio Library Data:
void AudioOutputFM::update(void)
{
  if (AudioOutputFM::block_left) release(AudioOutputFM::block_left);
  if (AudioOutputFM::block_right) release(AudioOutputFM::block_right);
  AudioOutputFM::block_left = receiveReadOnly(0); // input 0
  AudioOutputFM::block_right = receiveReadOnly(1); // input 1
}

//DMA interrupt:
void AudioOutputFM::dmaISR()
{
  dma.clearInterrupt();

  unsigned long us = micros();

  if ((uintptr_t)dma.TCD->SADDR < (uintptr_t)&fm_tx_buffer[I_NUM_SAMPLES]) {
    // DMA is transmitting the first half of the buffer
    // so we must fill the second half
    process(AudioOutputFM::block_left, AudioOutputFM::block_right, 0);
  } else {
    //fill the first half
    process(AudioOutputFM::block_left, AudioOutputFM::block_right, NUM_SAMPLES);
    if (AudioOutputFM::update_responsibility) AudioStream::update_all();
  }

  us = 2 * ( micros() - us );
  if (us > AudioOutputFM::us) AudioOutputFM::us = us;

  asm("dsb");
}


//Translates a sample to PLL multiplicator:
static inline __attribute__ ((pure))
uint32_t calcPLLnmult(float fsample)
{
  double fs = FS + FM_deviation * fsample;
  const unsigned n1 = 2; //SAI prescaler
  unsigned n2 = 1 + (24000000.0 * 27) / (fs * n1);

  double C = (fs * (n1 * n2)) / 24000000.0;
  unsigned nfact = C;
  uint32_t nmult = C * ndiv - (nfact * ndiv);
  return nmult;
}

//Called for every (half-)block of samples:
static
void process(audio_block_t *blockL, audio_block_t *blockR, unsigned offset)
{
  if (blockL == nullptr) return;

  float bL[NUM_SAMPLES];
  float bR[NUM_SAMPLES];

  //convert 16 bit samples to float & normalize
  if (blockR != nullptr) {
    for (unsigned idx = 0; idx < NUM_SAMPLES; idx++) {
      bL[idx] = blockL->data[idx + offset] / 32768.0f;
      bR[idx] = blockR->data[idx + offset] / 32768.0f;
    }
  } else {
    //blockR not filled: copy left channel.
    for (unsigned idx = 0; idx < NUM_SAMPLES; idx++) {
      bL[idx] = blockL->data[idx + offset] / 32768.0f;
      bR[idx] = bL[idx];
    }
  }


  //interpolation:
#if INTERPOLATION > 1
  fdata_t iL, iR;

#if 0 
  //simple - just copy the same sample 4 times (testing only)
  for (unsigned i; i< NUM_SAMPLES; i++) {
    iL[i*4] = iL[i*4 + 1] = iL[i*4 + 2] = iL[i*4 + 3] = bL[i];
    iR[i*4] = iR[i*4 + 1] = iR[i*4 + 2] = iR[i*4 + 3] = bR[i];
  }
#else
  // interpolate
  arm_fir_interpolate_f32(&interpolationL, bL, iL, NUM_SAMPLES);
  arm_fir_interpolate_f32(&interpolationR, bR, iR, NUM_SAMPLES);
  //Scaling after interpolation
  arm_scale_f32(iL, (float)INTERPOLATION, iL, I_NUM_SAMPLES);
  arm_scale_f32(iR, (float)INTERPOLATION, iR, I_NUM_SAMPLES);
#endif

  offset *= INTERPOLATION;
  if (AudioOutputFM::mono) {
    processMono(iL, iR, offset);
  } else {
    processStereo(iL, iR, offset);
  }

#else

  if (AudioOutputFM::mono) {
    processMono(bL, bR, offset);
  } else {
    processStereo(bL, bR, offset);
  }

#endif


  //Flush cache to memory, so that DMA sees the new data
  arm_dcache_flush_delete(&fm_tx_buffer[offset], sizeof(fm_tx_buffer) / 2 );
}




static void processMono(fdata_t blockL, fdata_t blockR, const unsigned offset)
{

  static float lastInputSample = 0;
  float sample;

  for (unsigned idx = 0; idx < I_NUM_SAMPLES; idx++)
  {
    sample = (blockL[idx] + blockR[idx]) / 2.0f;

#if !PREEMPHASIS_DISABLE
    float tmp = sample;

    // pre-emphasis filter: https://jontio.zapto.org/download/preempiir.pdf
    // https://github.com/jontio/JMPX/blob/master/libJMPX/JDSP.cpp

    sample = pre_a0 * sample + pre_a1 * lastInputSample + pre_b * sample;
    lastInputSample = tmp;
#endif

    fm_tx_buffer[idx + offset] = calcPLLnmult(sample);
  }
}



static void processStereo(fdata_t blockL, fdata_t blockR, const unsigned offset)
{
  //TBD: pre-emphasis
  //TBD: add pilot-tone, process stereo
  //TBD: RDS(?)
}


#endif
