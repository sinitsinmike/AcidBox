#include "config.h"
#include "driver/i2s.h"
//#include "delayline.h"
#include "synthvoice.h"
#include "sampler.h"
#include <Wire.h>
//#include <WiFi.h>

#ifdef MIDI_ON
  #include <MIDI.h>
  #ifdef MIDI_VIA_SERIAL
    // default settings for Hairless midi is 115200 8-N-1
    struct CustomBaudRateSettings : public MIDI_NAMESPACE::DefaultSerialSettings {
      static const long BaudRate = 115200;
    };
    MIDI_NAMESPACE::SerialMIDI<HardwareSerial, CustomBaudRateSettings> serialMIDI(Serial);
    MIDI_NAMESPACE::MidiInterface<MIDI_NAMESPACE::SerialMIDI<HardwareSerial, CustomBaudRateSettings>> MIDI((MIDI_NAMESPACE::SerialMIDI<HardwareSerial, CustomBaudRateSettings>&)serialMIDI);
  #else
    // MIDI port on UART2,   pins 16 (RX) and 17 (TX) prohibited, as they are used for PSRAM
    
  struct Serial2MIDISettings : public midi::DefaultSettings{
    static const long BaudRate = 31250;
    static const int8_t RxPin  = MIDIRX_PIN;
    static const int8_t TxPin  = MIDITX_PIN; 
  };
  
  HardwareSerial MIDISerial(2);
  MIDI_CREATE_CUSTOM_INSTANCE( HardwareSerial, MIDISerial, MIDI, Serial2MIDISettings );
  
  #endif
#endif


#ifdef SSD1306
#include <Adafruit_SSD1306.h>
#define SCREEN_ADDRESS 0x3C // The Address was discovered using the I2C Scanner
Adafruit_SSD1306 oled( 128, 64, &Wire, -1 );
#endif

#ifdef SH1106
#include <Adafruit_SH110X.h>
#define SCREEN_ADDRESS 0x3C // The Address was discovered using the I2C Scanner
Adafruit_SH1106G oled = Adafruit_SH1106G( 128, 64, &Wire, -1 );
#endif

const i2s_port_t i2s_num = I2S_NUM_0; // i2s port number

static float midi_pitches[128];
static float midi_phase_steps[128];
static float midi_2048_steps[128];
static float saw_2048[2048];
static float square_2048[2048];
static int debug_note=69;
TaskHandle_t SynthTask1;
TaskHandle_t SynthTask2;

// Output buffers (2ch interleaved)
static float synth_buf[2][DMA_BUF_LEN]; // 2 * 303 mono
static float drums_buf[DMA_BUF_LEN*2]; //  808 stereo 
static float mix_buf_l[DMA_BUF_LEN]; // pre-mix L and R channels
static float mix_buf_r[DMA_BUF_LEN]; // pre-mix L and R channels
static union { // a dirty trick, instead of true converting
  int16_t _signed[DMA_BUF_LEN * 2];
  uint16_t _unsigned[DMA_BUF_LEN * 2];
} out_buf;


size_t bytes_written;
static uint32_t c1=0, c2=0, c3=0, d1=0, d2=0, d3=0; //debug

//DelayLine <float, (size_t)20000> Delay;
SynthVoice Synth1(0); // use synth_buf[0]
SynthVoice Synth2(1); // use synth_buf[1]
Sampler Drums(5);

// Core0 task
static void audio_task1(void *userData) {

  Synth1.Init();  
  Synth2.Init();
  
    while(1) {
        // this part of the code never intersects with mixer()
        c1=micros();
        Synth1.Generate(); 
        Synth2.Generate();
        d1=micros()-c1;
        // this part of the code is operating with shared resources, so we should make it safe
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)){
          
            xTaskNotifyGive(SynthTask2); // if you have glitches, you may want to place this string in the end of audio_task1
        }        
    }
}

// task for Core1, which tipically runs user's code on ESP32
static void audio_task2(void *userData) {
  
  Effect_Init();
  Effect_SetBitCrusher( 0.0f );
  Reverb_Setup();  
  Delay_Init();  
  Drums.Init();
    while(1) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
          // we can run it together with synth(), but not with mixer()
          c2=micros();
          drums();
          d2 = micros() - c2;
          
          xTaskNotifyGive(SynthTask1);
          
          c3=micros();
          mixer();
          global_fx();
          d3 = micros() - c3;
          
          i2s_output();
        }
        taskYIELD();
    }
}

void setup(void) {

#ifdef MIDI_ON
  #ifdef MIDI_VIA_SERIAL 
    Serial.begin(115200); 
  #else
    pinMode( MIDIRX_PIN , INPUT_PULLDOWN); 
    MIDISerial.begin( 31250, SERIAL_8N1, MIDIRX_PIN, MIDITX_PIN ); // midi port
  #endif
#else
#endif

#ifdef DEBUG_ON
#ifndef MIDI_VIA_SERIAL
  Serial.begin(115200);
#endif
#endif

  for (uint8_t i = 0; i < GPIO_BUTTONS; i++) {
    pinMode(buttonGPIOs[i], INPUT_PULLDOWN);
  }
  
  buildTables();
 // WiFi.mode(WIFI_OFF);
  btStop();
  
#ifdef MIDI_ON
  MIDI.setHandleNoteOn(handleNoteOn);
  MIDI.setHandleNoteOff(handleNoteOff);
  MIDI.setHandleControlChange(handleCC);
  MIDI.setHandleProgramChange(handleProgramChange);
  MIDI.begin(MIDI_CHANNEL_OMNI);
#endif


#if defined SH1106 || defined SSD1306
  oledInit();
#endif 
 // silence while we haven't loaded anything reasonable
  for (int i=0; i < DMA_BUF_LEN; i++) { 
    drums_buf[i] = 0.0f ;
    drums_buf[i+DMA_BUF_LEN] = 0.0f ; 
    synth_buf[0][i] = 0.0f ; 
    synth_buf[1][i] = 0.0f ; 
    out_buf._signed[i*2] = 0 ;
    out_buf._signed[i*2+1] = 0 ;
    mix_buf_l[i] = 0.0f;
    mix_buf_r[i] = 0.0f;
  }
  
  i2sInit();
  i2s_write(i2s_num, out_buf._unsigned, sizeof(out_buf._unsigned), &bytes_written, portMAX_DELAY);
  
  xTaskCreatePinnedToCore( audio_task1, "SynthTask1", 12000, NULL, 1, &SynthTask1, 0 );
  xTaskCreatePinnedToCore( audio_task2, "SynthTask2", 12000, NULL, 1, &SynthTask2, 1 );
  
  // somehow we should allow tasks to run
  xTaskNotifyGive(SynthTask1);
  xTaskNotifyGive(SynthTask2);

}

void loop() { // default loopTask running on the Core1
  // you can still place some of your code here
  // or vTaskDelete();

#ifdef MIDI_ON
  MIDI.read();
#endif
 // processButtons();
 
  DEB (d1);
  DEB(" ");
  DEB (d2);
  DEB(" ");
  DEBUG (d3);
  
  taskYIELD(); // breath for all the rest of the Core1 tasks
}
