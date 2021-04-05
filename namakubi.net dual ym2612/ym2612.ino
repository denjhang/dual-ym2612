/* ym2612 sound driver
 rnx
 */

#include <Encoder.h>
#include <MIDI.h>
#include <Wire.h>
#include <SPI.h>

#include <SoftwareSerial.h> //needed for RS232 display, if used

#define disk1 0x51 //address of 32k eeprom chip
#define disk2 0x50 //address of 32k eeprom chip
#define INST_STRIDE 72 //size of instrument data in bytes

#define CH1 0 //magic constant for channel 1
#define CH2 1 //magic constant for channel 2
#define CH3 2 //magic constant for channel 3
#define CH4 4 //magic constant for channel 4
#define CH5 5 //magic constant for channel 5
#define CH6 6 //magic constant for channel 6
#define CH7 7 //magic constant for channel 7
#define CH8 8 //magic constant for channel 8
#define CH9 9 //magic constant for channel 9
#define CH10 11 //magic constant for channel 10
#define CH11 12 //magic constant for channel 11
#define CH12 13 //magic constant for channel 12

#define BANK1 0
#define BANK2 1

#define MODE_MONO 0
#define MODE_POLY 1

//the Operator struct contains all parameters of an Operator
typedef struct
{
  byte dt1;
  byte mul;
  byte tl;
  byte rs;
  byte ar;
  byte ams;
  byte d1r;
  byte d2r;
  byte d1l;
  byte rr;
  byte lfo;
} 
Operator;

//a Voice contains 4 Operators and a couple extra parameters
//this represents a single instrument
typedef struct
{
  Operator op[4];
  byte feedback; //AKA FL in OPM
  byte algorithm; //AKA CON in OPM
}
Voice;

//TODO: this is some bad code, fix this
Voice voice[14];

//This struct contains all 12 channels, 1-indexed for ease of use
//CHANNEL[1] is always channel 1, CHANNEL[4] is always channel 4, etc.
//CHANNEL[0] is invalid, don't use it!
const unsigned char CHANNEL[13] = {
  -1, CH1, CH2, CH3, CH4, CH5, CH6, CH7, CH8, CH9, CH10, CH11, CH12};

const int LCDPin = 25; //pin to use for sending data to the LCD screen

const int BUSPIN = 10; //8 data pins starting here

const int IRQPIN = 18; //timer interrupt pin

byte chctl = 0;

SoftwareSerial LCD = SoftwareSerial(0, LCDPin);

/* Chip select. Active-low. When high, this makes the data pins
 high-impedance, removing it from the bus and freeing the pins. */
const int CS2 = 40; //chip select ym2612 #1
const int CS = 45; //chip select ym2612 #2

const int AA1 = 41; //A1 bit, active high
const int AA0 = 42; //A0 bit, active high
const int RD = 43; //read bit, active low
const int WR = 44; //write bit, active low

const int SSP = 5; //Chip select for Sram

const int RESET = 6; //reset bit, active low

//timers for VGM mode. Not super useful
volatile int TIMER1 = LOW;
elapsedMicros TIMER;
unsigned long DTIMER = 0;
unsigned char timerreg = 0;

//Tracking globals to handle midi program change events
byte PROGRAMCHANGE = 0;
byte progchange[2];


/* Primary frequency table, in fnums (see sega2.doc)*/
//C4 is setpitch(3, freq[1], ...)
//SET BY EAR!
//FIXME: calculate proper numbers by tuning with an oscope and using sethz().
const unsigned int freq[13] = {
  617, 653, 692, 733, 777, 823, 872, 924, 979, 1037, 1099, 1164, 1232};

/*
  Some bad code. See the static definition of CH1-CH12.
 Channels 3 and 10 are not used so they're set to something dumb.
 */
unsigned int KEYS[14] = {
  0, 0, 0, 0xFFFD, 0, 0, 0, 0, 0, 0, 0xFFFC, 0, 0, 0};

//Frequency that each KEY is operating at (for pitch bend or something)
unsigned int KEYFREQ[14] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0};

//This represents an octave. Or something. I didn't know what I was doing when I wrote this
//it's basically lucky cargo cult magic, look but don't touch
unsigned int KEYBLOCK[14] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0};

//If this is MODE_MONO instead of MODE_POLY,
//each YM2612 channel will get its own midi channel
unsigned int MODE = MODE_POLY;

/* Sets 8-bit bus to write mode. */
void output()
{
  DDRC = 0xFF;
}

/*
  Sets 8-bit bus to read mode 
 This doesn't actually do anything useful
 */
void input()
{
  int i=BUSPIN;
  for(i;i<(BUSPIN + 8);i++)
  {
    pinMode(i, INPUT);
  }
}

/* Sets pin state on the 8-bit data bus. */
void writebus(unsigned char state)
{
  output();
  PORTC = state;
}

/*
  Reads pin state of the 8-bit data bus
 This doesn't actually do anything useful
 */
unsigned int readbus()
{
  unsigned int state=0;
  int i=BUSPIN;

  input();
  for(i;i<(i+8);i++)
  {
    if( digitalRead(i) == 1)
    {
      state = state ^ 1<<i;
    }
  }
  return(state);
}

//* Selects a register "reg" in the YM2612 */
//FIXME: combine selectreg and selectreg2
void selectreg(unsigned char reg)
{
  /*digitalWrite(CS, LOW); //40
   digitalWrite(RD, HIGH); //43
   digitalWrite(WR, HIGH); //44
   digitalWrite(AA0, LOW); //42
   digitalWrite(AA1, LOW); //41*/
  PORTF = B11100000;
  delayMicroseconds(2);
  writebus(reg);
  PORTF = B10100000;
  PORTF = B11100000;
  PORTF = B11100100;
  /*digitalWrite(WR, LOW);
   digitalWrite(WR, HIGH);
   digitalWrite(CS, HIGH);*/
}

/* Selects a register "reg" in the YM2612 */
void selectreg2(unsigned char reg)
{
  /*  digitalWrite(CS, LOW);
   digitalWrite(RD, HIGH);
   digitalWrite(WR, HIGH);
   digitalWrite(AA0, LOW);
   digitalWrite(AA1, HIGH);*/
  PORTF = B11101000;
  delayMicroseconds(1);
  writebus(reg);  
  PORTF = B10101000;
  PORTF = B11101000;
  PORTF = B11101100;
  /*  digitalWrite(WR, LOW);
   digitalWrite(WR, HIGH);
   digitalWrite(CS, HIGH);*/
  //delayMicroseconds(10);
}

void selectreg3(unsigned char reg, unsigned int bank, unsigned int chip)
{
  unsigned int chipselect = 0;

  if ( chip == 1)
  {
    chipselect = CS;
  }
  else
  {
    chipselect = CS2;
  }

  digitalWrite(chipselect, LOW);
  digitalWrite(RD, HIGH);
  digitalWrite(WR, HIGH);

  if (bank == BANK1)
  {
    digitalWrite(AA0, LOW);
    digitalWrite(AA1, LOW);
  }
  else if (bank == BANK2)
  {
    digitalWrite(AA0, LOW);
    digitalWrite(AA1, HIGH);
  }

  writebus(reg);
  digitalWrite(WR, LOW);
  digitalWrite(WR, HIGH);
  digitalWrite(chipselect, HIGH);
}

/* Writes "state" to the currently selected register in bank 1. */
//FIXME: combine writereg and writereg2
void writereg(unsigned char state)
{
  /*digitalWrite(CS, LOW); //40
   digitalWrite(RD, HIGH); //43
   digitalWrite(WR, HIGH); //44
   digitalWrite(AA1, LOW); //41
   digitalWrite(AA0, HIGH);//42*/
  PORTF = B11110000;
  delayMicroseconds(2);

  writebus(state);
  PORTF = B10110000;
  PORTF = B11110000;
  PORTF = B11110100;
  /*digitalWrite(WR, LOW);
   digitalWrite(WR, HIGH);
   digitalWrite(CS, HIGH);*/
}

/* Writes "state" to the currently selected register in bank 2. */
void writereg2(unsigned char state)
{
  /*digitalWrite(CS, LOW);
   digitalWrite(RD, HIGH);
   digitalWrite(WR, HIGH);
   digitalWrite(AA1, HIGH);
   digitalWrite(AA0, HIGH);*/
  PORTF = B11111000;
  delayMicroseconds(2);

  writebus(state);
  PORTF = B10111000;
  PORTF = B11111000;
  PORTF = B11111100;

  /*digitalWrite(WR, LOW);
   digitalWrite(WR, HIGH);
   digitalWrite(CS, HIGH);*/
  //delayMicroseconds(10);
}

void writereg3(unsigned char state, unsigned int bank, unsigned int chip)
{
  unsigned int chipselect = 0;

  if ( chip == 1)
  {
    chipselect = CS;
  }
  else
  {
    chipselect = CS2;
  }

  digitalWrite(chipselect, LOW);
  digitalWrite(RD, HIGH);
  digitalWrite(WR, HIGH);

  if (bank == BANK1)
  {
    digitalWrite(AA1, LOW);
    digitalWrite(AA0, HIGH);
  }
  else if (bank == BANK2)
  {
    digitalWrite(AA1, HIGH);
    digitalWrite(AA0, HIGH);
  }

  writebus(state);
  digitalWrite(WR, LOW);
  digitalWrite(WR, HIGH);
  digitalWrite(chipselect, HIGH);
  //delayMicroseconds(10);
}

/* Writes "state" to the register "address" */
//old function, only used in VGM player.
void reg(unsigned char address, unsigned char state, unsigned char chip)
{
  selectreg3(address, BANK1, chip);
  writereg3(state, BANK1, chip);
}
//same as reg except for bank 2
void reg2(unsigned char address, unsigned char state)
{
  selectreg3(address, BANK2, 1);
  writereg3(state, BANK2, 1);
}

/*
  Proper register write function that can handle two chips 
 this is the one to use
 */
void reg3(unsigned int address, unsigned int state, unsigned char channel)
{
  unsigned char bank;
  unsigned char chip;

  chip = 1;

  if (channel > CH6)
  {
    chip = 2;
  }

  if (channel <= CH3)
  {
    bank = BANK1;
  }
  else if (channel <= CH6)
  {
    bank = BANK2;
  }
  else if (channel <= CH9)
  {
    bank = BANK1;
  }
  else if (channel <= CH12)
  {
    bank = BANK2; 
  }

  selectreg3(address, bank, chip);
  writereg3(state, bank, chip);
}

/* Selects and loads an instrument from the EEPROM. */
void selectinst(unsigned int instrument, unsigned char channel)
{
  unsigned int inst = instrument * INST_STRIDE;
  unsigned char buf = 0;
  unsigned char offset;

  if (channel <= CH3)
  {
    offset = channel;
  }
  else if (channel <= CH6)
  {
    offset = channel - 4;
  }
  else if (channel <= CH9)
  {
    offset = channel - 7;
  }
  else if (channel <= CH12)
  {
    offset = channel -11;
  }

  reg3(0x28, 0x00 + channel, channel); //Mute channel before modifying it

  buf = readEEPROM(disk1, inst + 36); //DT1 (3 bits)
  voice[channel].op[0].dt1 = buf;
  buf <<= 4;
  voice[channel].op[0].mul = readEEPROM(disk1, inst + 35); //MUL (nibble)
  buf += voice[channel].op[0].mul;
  reg3(0x30 + offset, buf, channel); //Detune + Multiply Operator 1

  buf = readEEPROM(disk1, inst + 47); //DT1
  voice[channel].op[1].dt1 = buf;
  buf <<= 4;
  voice[channel].op[1].mul = readEEPROM(disk1, inst + 46); //MUL
  buf += voice[channel].op[1].mul;
  reg3(0x34 + offset, buf, channel); //Detune + Multiply Operator 2

  buf = readEEPROM(disk1, inst + 58); //DT1
  voice[channel].op[2].dt1 = buf;
  buf <<= 4;
  voice[channel].op[2].mul = readEEPROM(disk1, inst + 57); //MUL
  buf += voice[channel].op[2].mul;
  reg3(0x38 + offset, buf, channel); //Detune + Multiply Operator 3

  buf = readEEPROM(disk1, inst + 69); //DT1
  voice[channel].op[3].dt1 = buf;
  buf <<= 4;
  voice[channel].op[3].mul = readEEPROM(disk1, inst + 68); //MUL
  buf += voice[channel].op[3].mul;
  reg3(0x3C + offset, buf, channel); //Detune + Multiply Operator 4

  buf = readEEPROM(disk1, inst + 33); //TL
  voice[channel].op[0].tl = buf;
  reg3(0x40 + offset, buf, channel); //Total Level Operator 1
  buf = readEEPROM(disk1, inst + 44); //TL
  voice[channel].op[1].tl = buf;
  reg3(0x44 + offset, buf, channel); //Total Level Operator 2
  buf = readEEPROM(disk1, inst + 55); //TL
  voice[channel].op[2].tl = buf;
  reg3(0x48 + offset, buf, channel); //Total Level Operator 3
  buf = readEEPROM(disk1, inst + 66); //TL
  voice[channel].op[3].tl = buf;
  reg3(0x4C + offset, buf, channel); //Total Level Operator 4

  buf = readEEPROM(disk1, inst + 34); //RS
  voice[channel].op[0].rs = buf;
  buf <<= 6;
  voice[channel].op[0].ar = readEEPROM(disk1, inst + 28); //AR
  buf += voice[channel].op[0].ar;
  reg3(0x50 + offset, buf, channel); //Rate Scaling + Attack Rate Operator 1

  buf = readEEPROM(disk1, inst + 45); //RS
  voice[channel].op[1].rs = buf;
  buf <<= 6;
  voice[channel].op[1].ar = readEEPROM(disk1, inst + 39); //AR
  buf += voice[channel].op[1].ar;
  reg3(0x54 + offset, buf, channel); //Rate Scaling + Attack Rate Operator 2

  buf = readEEPROM(disk1, inst + 56); //RS
  voice[channel].op[2].rs = buf;
  buf <<= 6;
  voice[channel].op[2].ar = readEEPROM(disk1, inst + 50); //AR
  buf += voice[channel].op[2].ar;
  reg3(0x58 + offset, buf, channel); //Rate Scaling + Attack Rate Operator 3

  buf = readEEPROM(disk1, inst + 67); //RS
  voice[channel].op[3].rs = buf;
  buf <<= 6;
  voice[channel].op[3].ar = readEEPROM(disk1, inst + 61); //AR
  buf += voice[channel].op[3].ar;
  reg3(0x5C + offset, buf, channel); //Rate Scaling + Attack Rate Operator 4

  buf = readEEPROM(disk1, inst + 38); //AMS-EN
  voice[channel].op[0].ams = buf;
  buf <<= 7;
  voice[channel].op[0].d1r = readEEPROM(disk1, inst + 29); //D1R
  buf += voice[channel].op[0].d1r;
  reg3(0x60 + offset, buf, channel); //Decay rate + Amplitude Modulation, Operator 1
  buf = readEEPROM(disk1, inst + 49); //AMS-EN
  voice[channel].op[1].ams = buf;
  buf <<= 7;
  voice[channel].op[1].d1r = readEEPROM(disk1, inst + 40); //D1R
  buf += voice[channel].op[1].d1r;
  reg3(0x64 + offset, buf, channel); //Decay rate (keyon) Operator 2
  buf = readEEPROM(disk1, inst + 60); //AMS-EN
  voice[channel].op[2].ams = buf;  
  buf <<= 7;
  voice[channel].op[2].d1r= readEEPROM(disk1, inst + 51); //D1R
  buf += voice[channel].op[2].d1r;
  reg3(0x68 + offset, buf, channel); //Decay rate (keyon) Operator 3
  buf = readEEPROM(disk1, inst + 71); //AMS-EN
  voice[channel].op[3].ams = buf;
  buf <<= 7;
  voice[channel].op[3].d1r = readEEPROM(disk1, inst + 62); //D1R
  buf += voice[channel].op[3].d1r;
  reg3(0x6C + offset, buf, channel); //Decay rate (keyon) Operator 4

  buf = readEEPROM(disk1, inst + 30); //D2R (low nibble)
  voice[channel].op[0].d2r = buf;
  reg3(0x70 + offset, buf, channel); //Secondary decay (keyon) Operator 1
  buf = readEEPROM(disk1, inst + 41); //D2R
  voice[channel].op[1].d2r = buf;
  reg3(0x74 + offset, buf, channel); //Secondary decay (keyon) Operator 2
  buf = readEEPROM(disk1, inst + 52); //D2R
  voice[channel].op[2].d2r = buf;
  reg3(0x78 + offset, buf, channel); //Secondary decay (keyon) Operator 3
  buf = readEEPROM(disk1, inst + 63); //D2R
  voice[channel].op[3].d2r = buf;
  reg3(0x7C + offset, buf, channel); //Secondary decay (keyon) Operator 4

  buf = readEEPROM(disk1, inst + 32) << 4; //D1L (high nibble)
  buf += readEEPROM(disk1, inst + 31); //RR (low nibble)
  reg3(0x80 + offset, buf, channel); //Secondary amplitude + release rate Operator 1
  buf = readEEPROM(disk1, inst + 43) << 4; //D1L (high nibble)
  buf += readEEPROM(disk1, inst + 42); //RR (low nibble)
  reg3(0x84 + offset, buf, channel); //Secondary amplitude/release rate Operator 2
  buf = readEEPROM(disk1, inst + 54) << 4; //D1L (high nibble)
  buf += readEEPROM(disk1, inst + 53); //RR (low nibble)
  reg3(0x88 + offset, buf, channel); //Secondary amplitude/release rate Operator 3
  buf = readEEPROM(disk1, inst + 65) << 4; //D1L (high nibble)
  buf += readEEPROM(disk1, inst + 64); //RR (low nibble)
  reg3(0x8C + offset, buf, channel); //Secondary amplitude/release rate Operator 4

  buf = readEEPROM(disk1, inst + 22); //FL, feedback (3 bits)
  voice[channel].feedback = buf;
  buf <<= 3;
  voice[channel].algorithm = readEEPROM(disk1, inst + 23); //CON, algorithm (3 bits)
  buf += voice[channel].algorithm;
  reg3(0xB0 + offset, buf, channel); //Feedback/algorithm select

  reg3(0xB4 + offset, 0xC0, channel); //Both speakers on
  reg3(0x28, channel, channel); //Key off
}

/* Copies an instrument from channel 0 to the specified channel. */
//TODO: rename this, this name is stupid
//TODO: this function is unfinished
void selectvoice(unsigned char channel)
{
  unsigned char buf = 0;
  unsigned char offset;

  if (channel <= CH3)
  {
    offset = channel;
  }
  else if (channel <= CH6)
  {
    offset = channel - 4;
  }
  else if (channel <= CH9)
  {
    offset = channel - 7;
  }
  else if (channel <= CH12)
  {
    offset = channel -11;
  }

  reg3(0x28, 0x00 + channel, channel); //Mute channel before modifying it

  buf = voice[0].op[0].dt1; //DT1
  voice[channel].op[0].dt1 = buf;
  buf <<= 4;
  voice[channel].op[0].mul = voice[0].op[0].mul; //MUL
  buf += voice[0].op[0].mul;
  reg3(0x30 + offset, buf, channel); //Detune + Multiply Operator 1

  buf = voice[0].op[1].dt1; //DT1
  voice[channel].op[1].dt1 = buf;
  buf <<= 4;
  voice[channel].op[1].mul = voice[0].op[1].mul; //MUL
  buf += voice[0].op[1].mul;
  reg3(0x34 + offset, buf, channel); //Detune + Multiply Operator 2

  buf = voice[0].op[2].dt1; //DT1
  voice[channel].op[2].dt1 = buf;
  buf <<= 4;
  voice[channel].op[2].mul = voice[0].op[2].mul; //MUL
  buf += voice[0].op[2].mul;
  reg3(0x38 + offset, buf, channel); //Detune + Multiply Operator 3

  buf = voice[0].op[3].dt1; //DT1
  voice[channel].op[3].dt1 = buf;
  buf <<= 4;
  voice[channel].op[3].mul = voice[0].op[3].mul; //MUL
  buf += voice[0].op[3].mul;
  reg3(0x3C + offset, buf, channel); //Detune + Multiply Operator 4

  buf = voice[0].op[0].tl; //TL
  voice[channel].op[0].tl = buf;
  reg3(0x40 + offset, buf, channel); //Total Level Operator 1
  buf = voice[0].op[1].tl; //TL
  voice[channel].op[1].tl = buf;
  reg3(0x44 + offset, buf, channel); //Total Level Operator 2
  buf = voice[0].op[2].tl; //TL
  voice[channel].op[2].tl = buf;
  reg3(0x48 + offset, buf, channel); //Total Level Operator 3
  buf = voice[0].op[3].tl; //TL
  voice[channel].op[3].tl = buf;
  reg3(0x4C + offset, buf, channel); //Total Level Operator 4

  buf = voice[0].op[0].rs; //RS
  voice[channel].op[0].rs = buf;
  buf <<= 6;
  voice[channel].op[0].ar = voice[0].op[0].ar; //AR
  buf += voice[channel].op[0].ar;
  reg3(0x50 + offset, buf, channel); //Rate Scaling + Attack Rate Operator 1

  buf = voice[0].op[1].rs; //RS
  voice[channel].op[1].rs = buf;
  buf <<= 6;
  voice[channel].op[1].ar = voice[0].op[1].ar; //AR
  buf += voice[channel].op[1].ar;
  reg3(0x54 + offset, buf, channel); //Rate Scaling + Attack Rate Operator 2

  buf = voice[0].op[2].rs; //RS
  voice[channel].op[2].rs = buf;
  buf <<= 6;
  voice[channel].op[2].ar = voice[0].op[2].ar; //AR
  buf += voice[channel].op[2].ar;
  reg3(0x58 + offset, buf, channel); //Rate Scaling + Attack Rate Operator 3

  buf = voice[0].op[3].rs; //RS
  voice[channel].op[3].rs = buf;
  buf <<= 6;
  voice[channel].op[3].ar = voice[0].op[3].ar; //AR
  buf += voice[channel].op[3].ar;
  reg3(0x5C + offset, buf, channel); //Rate Scaling + Attack Rate Operator 4

  buf = voice[0].op[0].ams; //AMS-EN
  voice[channel].op[0].ams = buf;
  buf <<= 7;
  voice[channel].op[0].d1r = voice[0].op[0].d1r; //D1R
  buf += voice[channel].op[0].d1r;
  reg3(0x60 + offset, buf, channel); //Decay rate + Amplitude Modulation, Operator 1
  buf = voice[0].op[1].ams; //AMS-EN
  voice[channel].op[1].ams = buf;
  buf <<= 7;
  voice[channel].op[1].d1r = voice[0].op[1].d1r; //D1R
  buf += voice[channel].op[1].d1r;
  reg3(0x64 + offset, buf, channel); //Decay rate (keyon) Operator 2
  buf = voice[0].op[2].ams; //AMS-EN
  voice[channel].op[2].ams = buf;  
  buf <<= 7;
  voice[channel].op[2].d1r= voice[0].op[2].d1r; //D1R
  buf += voice[channel].op[2].d1r;
  reg3(0x68 + offset, buf, channel); //Decay rate (keyon) Operator 3
  buf = voice[0].op[3].ams; //AMS-EN
  voice[channel].op[3].ams = buf;
  buf <<= 7;
  voice[channel].op[3].d1r = voice[0].op[3].d1r; //D1R
  buf += voice[channel].op[3].d1r;
  reg3(0x6C + offset, buf, channel); //Decay rate (keyon) Operator 4

  //RANIX
  //TODO: this function is unfinished
  /*
  buf = readEEPROM(disk1, inst + 30); //D2R (low nibble)
   voice[channel].op[0].d2r = buf;
   reg3(0x70 + offset, buf, channel); //Secondary decay (keyon) Operator 1
   buf = readEEPROM(disk1, inst + 41); //D2R
   voice[channel].op[1].d2r = buf;
   reg3(0x74 + offset, buf, channel); //Secondary decay (keyon) Operator 2
   buf = readEEPROM(disk1, inst + 52); //D2R
   voice[channel].op[2].d2r = buf;
   reg3(0x78 + offset, buf, channel); //Secondary decay (keyon) Operator 3
   buf = readEEPROM(disk1, inst + 63); //D2R
   voice[channel].op[3].d2r = buf;
   reg3(0x7C + offset, buf, channel); //Secondary decay (keyon) Operator 4
   
   buf = readEEPROM(disk1, inst + 32) << 4; //D1L (high nibble)
   buf += readEEPROM(disk1, inst + 31); //RR (low nibble)
   reg3(0x80 + offset, buf, channel); //Secondary amplitude + release rate Operator 1
   buf = readEEPROM(disk1, inst + 43) << 4; //D1L (high nibble)
   buf += readEEPROM(disk1, inst + 42); //RR (low nibble)
   reg3(0x84 + offset, buf, channel); //Secondary amplitude/release rate Operator 2
   buf = readEEPROM(disk1, inst + 54) << 4; //D1L (high nibble)
   buf += readEEPROM(disk1, inst + 53); //RR (low nibble)
   reg3(0x88 + offset, buf, channel); //Secondary amplitude/release rate Operator 3
   buf = readEEPROM(disk1, inst + 65) << 4; //D1L (high nibble)
   buf += readEEPROM(disk1, inst + 64); //RR (low nibble)
   reg3(0x8C + offset, buf, channel); //Secondary amplitude/release rate Operator 4
   
   buf = readEEPROM(disk1, inst + 22); //FL, feedback (3 bits)
   voice[channel].feedback = buf;
   buf <<= 3;
   voice[channel].algorithm = readEEPROM(disk1, inst + 23); //CON, algorithm (3 bits)
   buf += voice[channel].algorithm;
   reg3(0xB0 + offset, buf, channel); //Feedback/algorithm select
   
   reg3(0xB4 + offset, 0xC0, channel); //Both speakers on
   reg3(0x28, channel, channel); //Key off
   */
}

/* Reads status information from the YM2612
 
 Registers are READ-ONLY and you will ONLY get status.
 Returns:
 0: Ready to write.
 Anything else: Not ready.
 */
unsigned int readreg()
{
  unsigned int state = 0;

  digitalWrite(CS, LOW);
  digitalWrite(RD, HIGH);
  digitalWrite(WR, HIGH);
  digitalWrite(AA1, LOW);
  digitalWrite(AA0, HIGH);

  digitalWrite(RD, LOW);
  state = readbus();
  digitalWrite(RD, HIGH);
  digitalWrite(CS, HIGH);
  return(state);
}

/*
  Sets the pitch of a single channel to an fnumber
 (see sega2.doc for info on fnumbers).
 */
void setpitch(unsigned int block, unsigned int fnumber, unsigned int channel)
{
  int i;
  unsigned int state = 0;
  unsigned int state2 = 0;
  unsigned int tchan = channel;

  KEYFREQ[channel] = fnumber;
  KEYBLOCK[channel] = block;

  for(i=0; i<3; i++)
  {
    if( (block & 1<<i) != 0)
    {
      state |= 1<<(i+3);
    }
  }

  for(i=0; i<3; i++)
  {
    if( (fnumber & 1<<(i+8)) != 0)
    {
      state |= (1<<i);
    }
  }

  for(i=0; i<8; i++)
  {
    if( (fnumber & 1<<i) !=0)
    {
      state2 |= (1<<i);
    }
  }

  if (tchan > CH6) tchan = channel -7;

  //FIXME: comment this shit wtf
  if ( channel <= CH3 )
  {
    reg3( 0xA4 + tchan, state, channel);
    reg3( 0xA0 + tchan, state2, channel);
  }
  else if (channel <= CH6)
  {
    reg3( 0xA4 + tchan -4, state, channel);
    reg3( 0xA0 + tchan -4, state2, channel);
  }
  else if ( channel <= CH9 )
  {
    reg3( 0xA4 + tchan, state, channel);
    reg3( 0xA0 + tchan, state2, channel);
  }
  else if (channel <= CH12)
  {
    reg3( 0xA4 + tchan -4, state, channel);
    reg3( 0xA0 + tchan -4, state2, channel);
  }
}

/* Bad poorly-implemented pitch bend function */
void pitchbend(unsigned int bend, unsigned int channel)
{
  int i;
  unsigned int state = 0;
  unsigned int state2 = 0;
  unsigned int tchan = channel;
  unsigned int fnumber = KEYFREQ[channel] + bend;
  unsigned int block = KEYBLOCK[channel];

  for(i=0; i<3; i++)
  {
    if( (block & 1<<i) != 0)
    {
      state |= 1<<(i+3);
    }
  }

  for(i=0; i<3; i++)
  {
    if( (fnumber & 1<<(i+8)) != 0)
    {
      state |= (1<<i);
    }
  }

  for(i=0; i<8; i++)
  {
    if( (fnumber & 1<<i) !=0)
    {
      state2 |= (1<<i);
    }
  }

  if (tchan > CH6) tchan = channel -7;

  //FIXME: comment this shit wtf
  if ( channel <= CH3 )
  {
    reg3( 0xA4 + tchan, state, channel);
    reg3( 0xA0 + tchan, state2, channel);
  }
  else if (channel <= CH6)
  {
    reg3( 0xA4 + tchan -4, state, channel);
    reg3( 0xA0 + tchan -4, state2, channel);
  }
  else if ( channel <= CH9 )
  {
    reg3( 0xA4 + tchan, state, channel);
    reg3( 0xA0 + tchan, state2, channel);
  }
  else if (channel <= CH12)
  {
    reg3( 0xA4 + tchan -4, state, channel);
    reg3( 0xA0 + tchan -4, state2, channel);
  }
}

/*
  Selects a frequency in hz.
 
 This function works properly only with a 7.68mhz signal to the chips.
 The 10666.66 constant needs to be verified. If it's off the pitch will
 be a little bit wrong.
 */
void sethz(double hz)
{
  double fnum;
  int block = 3;

  while(1)
  {
    fnum = hz / pow(2, block - 21) / 106666.66; //FIXME: correct this constant
    if( fnum >=1250 )
    {
      block++;
    }
    else if( fnum <= 600 )
    {
      block--;
    }
    else break;
  }
  setpitch(block, fnum, CH1);
}

/* Plays a note on channel 1. */
//FIXME: Delete this, it's debug code
void note1()
{
  reg(0x28, 0xF0, 1);
  delay(50);
  reg(0x28, 0x00, 1);
  delay(50);
}

/* Play a note on channel 4. */
//FIXME: Delete this, it's debug code
/*
void note2()
 {
 reg(0x28, 0xF0 + CH4);
 delay(500);
 reg(0x28, CH4);
 delay(500);
 }
 */

/* Press a key. */
void keyDown(int channel, int note)
{
  int harbl, baz = 3;
  unsigned int chip;

  if (MODE == MODE_POLY)
  {
    for(int i=0; i<14; i++)
    {      
      if (KEYS[i] == 0)
      {
        channel = i;
        break;
      }
    }
  }
  if(KEYS[channel] != 0) return;

  KEYS[channel] = note;
  harbl = note / 12;
  harbl -= 1;
  baz = note % 12;

  setpitch(harbl, freq[baz+1], channel);

  if (channel <= CH6)
  {
    chip = 1;
  }
  else if (channel <= CH12)
  {
    chip = 2;
    channel -=7;
  }
  reg(0x28, 0xF0 + channel, chip);
}

/* Release a key. */
void keyUp(int channel, int note)
{
  unsigned int chip = 0;

  if (MODE == MODE_POLY)
  {
    for(int i=0;i<14;i++)
    {
      if( (channel == 3) || (channel == 10)) continue;
      if(KEYS[i] == note)
        channel = i;
    }
  }

  if(KEYS[channel] > 0)
  {
    KEYS[channel] = 0;

    if (channel <= CH6)
    {
      chip = 1;
    }
    else if (channel <= CH12)
    {
      chip = 2;
      channel -= 7;
    }
    reg(0x28, channel, chip);
  }
}

/* Wipes across a range of notes. */
//FIXME: delete this, it's debug code
void wipe()
{
  sethz(246.94);
  note1();
  sethz(261.63);
  note1();
  sethz(277.18);
  note1();
  sethz(293.66);
  note1();
  sethz(311.13);
  note1();
  sethz(329.63);
  note1();
  sethz(349.23);
  note1();
  sethz(369.99);
  note1();
  sethz(392.00);
  note1();
  sethz(415.30);
  note1();
  sethz(440.00);
  note1();
  sethz(466.16);
  note1();
  sethz(493.88);
  note1(); 
}

/* Write a byte to the 32k EEPROM. */
void writeEEPROM( unsigned char prom, unsigned int eeaddress, byte data ) 
{
  Wire.beginTransmission(prom);
  Wire.send((int)(eeaddress >> 8));   // MSB
  Wire.send((int)(eeaddress & 0xFF)); // LSB
  Wire.send(data);
  Wire.endTransmission();

  delay(5);
}

/* Read a byte from the 32k EEPROM. */
byte readEEPROM(unsigned char prom, unsigned int eeaddress) 
{
  byte rdata = 0xFF;

  Wire.beginTransmission(prom);
  Wire.send((int)(eeaddress >> 8));   // MSB
  Wire.send((int)(eeaddress & 0xFF)); // LSB
  Wire.endTransmission();

  Wire.requestFrom(disk1,1);

  if (Wire.available())
  {
    rdata = Wire.receive();
    return rdata;
  } 
  return 0;
}

/* Write a byte to the auxillary SRAM. */
void writeram(unsigned short address, unsigned char *data, unsigned int count)
{
  int i;

  PORTD &= ~(1 << 5);
  SPI.transfer(0x02);
  SPI.transfer((address & 0xFF00) >> 8);
  SPI.transfer(address & 0x00FF);
  for (i=0; i<count; i++)
  {
    SPI.transfer(*data);
  }
  PORTD |= 1 << 5;
}

/* Read a byte from the auxillary SRAM. */
void readram(unsigned short address, byte *buffer, unsigned int count)
{
  byte rec;
  int i;
  PORTD &= ~(1 << 5);
  delayMicroseconds(12);
  SPI.transfer(0x03);
  SPI.transfer((address & 0xFF00) >> 8);
  SPI.transfer(address & 0x00FF);
  for (i=0; i<count; i++)
  {
    buffer[i] = SPI.transfer(0);
  }
  PORTD |= 1 << 5;
}

/* Flash the 32k EEPROM with instrument data. */
void flashEEPROM()
{
  int ibyte = 0;
  int stride = 0;

  Serial.begin(921600);

  while(stride < 12456) //FIXME: error check this
    if (Serial.available() > 0)
    {
      ibyte = Serial.read();
      if (ibyte >= 0)
      {
        writeEEPROM(disk1, stride, (char)ibyte);
        stride++;
      }
    }   
}

/* This function is stupid, only used in VGM player */
void dtime(unsigned long time)
{
  if ( TIMER < DTIMER )
  {
    DTIMER += time;
  }
  else
  {
    TIMER = 0;
    DTIMER = time; 
  }
}

/* If uncommented, this will enable a crude VGM player
 void vgmplay()
 {
 unsigned char ibyte = 0;
 int rdy = 0;
 int n = 0;
 unsigned char ibyte2 = 0;
 unsigned char buff[3280];
 unsigned char* buf1 = buff;
 unsigned char* buf = buf1;
 unsigned char* buf2 = buff + 64;
 byte ram[128];
 byte * ram1 = ram;
 unsigned short ramptr = 0;
 int more = 0;
 int cnt = 0;
 int lize = 0;
 int errcnt = 0;
 int exit = 0;
 
 
 LCD.print("VGM Player Mode");
 lcdline2();
 
 while(1)
 { 
 if ( more == 0)
 {
 while( !(RawHID.recv(buf1, 0) > 0) );
 buf = buf1;
 }
 else
 {
 if (buf1 >= buff + (1280 - 65))
 {
 buf1 = buff;
 buf = buf1;
 }
 else
 {
 buf1 += 64;
 buf = buf1;
 }
 more--;
 }
 
 exit = 0;
 
 while (exit < 3)
 {
 //check timer, wait if necessary
 while ( TIMER < DTIMER)
 {
 if ( (DTIMER > (TIMER + 100))  )
 {
 if ( (more < 18) )
 {
 if (RawHID.recv(buf2, 0) > 0)
 {
 if ( buf2 >= buff + (1280 -65))
 {
 buf2 = buff;
 }
 else buf2 += 64;
 more++;
 }
 if (buf2 == buf1)
 {
 lcdinit();
 LCD.print("MASSIVE ERROR");
 }
 }
 }
 }
 
 TIMER = 0;
 DTIMER = 0;
 
 if (*buf == 39)
 {
 buf++; 
 if (*buf == 42)
 {
 buf++;
 if (*buf == 26)
 {
 exit = 3;
 continue;
 }
 else
 {
 buf -= 2; 
 }
 }
 else
 {
 buf--;
 }          
 }
 
 if ((*buf & 0xF0) == 0x60)
 {
 dtime((*buf &0x0F) * 20.6757);
 //buf++;
 //ibyte = *buf;
 if (ram1 == ram + 127)
 {
 readram(ramptr, ram, 128);
 ram1 = ram;
 }
 
 selectreg(0x2A);
 writereg(*ram1);
 ram1++;
 ramptr++;
 buf++;
 continue;
 }
 
 switch(*buf)
 {
 case 1: //register 1 write
 buf++;
 ibyte = *buf;
 buf++;
 ibyte2 = *buf;
 
 if(ibyte == 0x28)
 {
 if ( lize == 0) lize = 1;
 }
 
 if (lize == 0) delay(2);
 selectreg(ibyte);
 writereg(ibyte2);
 break;
 case 2: //register 2 write
 buf++;
 ibyte = *buf;
 buf++;
 ibyte2 = *buf;
 if (lize == 0) delay(2);
 selectreg2(ibyte);
 writereg2(ibyte2);
 break;
 case 3: //incoming PCM data
 buf++;
 writeram(ramptr, buf, 1);
 ramptr++;
 break;
 case 4: //1 byte wait data
 buf++;
 ibyte = *buf;
 dtime(ibyte * 12.6757);
 break;
 case 5: //2 byte wait data
 unsigned int wait;
 buf++;
 ibyte = *buf;
 buf++;
 ibyte2 = *buf;
 wait = (ibyte << 8) + ibyte2;
 dtime((unsigned long)(wait * 22.6757));
 break;
 //case 6 is used above in a pre-switch if
 case 7: //static time delay
 dtime(735 * 22.6757);
 break;
 case 8: //static time delay
 dtime(882 * 22.6757);
 break;
 case 9:
 ramptr = 0;
 break;
 case 10: //stream begin
 lize = 0;
 digitalWrite(RESET, LOW);
 delay(100);
 digitalWrite(RESET, HIGH); //Active low
 delay(100);
 ramptr = 0;
 break;
 case 11:
 buf++;
 ramptr = *buf << 8;
 buf++;
 ramptr += *buf;
 readram(ramptr, ram, 128);
 ram1 = ram;
 break;
 default:
 lcdinit();
 LCD.print("MAJOR ERROR");
 lcdline2();
 LCD.print(String(*buf, DEC));
 }
 buf++;
 }
 }  
 }
 */

/* Initializes the LCD screen */
void lcdinit()
{

  LCD.write((byte)0xFE);
  LCD.write((byte)0x51); //clear
  LCD.write((byte)0x00);

  delayMicroseconds(150);
}

/* I forgot what this does. Moves LCD cursor to line 2? */
void lcdline2()
{
  LCD.write((byte)0xFE);
  LCD.write((byte)0x45);
  LCD.write((byte)0x40);
}

/* Writes the string data to the LCD screen at the current cursor position */
void lcdwrite(const char* data)
{
  LCD.print(data);
  delayMicroseconds(150);  
}

/*
  Initialize the Teensy and the YM2612.
 This function is invoked automatically by the Teensy every boot.
 */
void setup()
{
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleNoteOn(HandleNoteOn);
  MIDI.setHandleNoteOff(HandleNoteOff);
  MIDI.setHandleControlChange(HandleControlChange);
  MIDI.setHandleProgramChange(HandleProgramChange);
  MIDI.setHandlePitchBend(HandlePitchBend);

  Wire.begin();

  pinMode(LCDPin, OUTPUT);
  pinMode(CS, OUTPUT);
  pinMode(CS2, OUTPUT);
  pinMode(RD, OUTPUT);
  pinMode(WR, OUTPUT);
  pinMode(AA0, OUTPUT);
  pinMode(AA1, OUTPUT);
  pinMode(RESET, OUTPUT);
  pinMode(SSP, OUTPUT);
  digitalWrite(SSP, HIGH);

  SPI.begin();
  SPI.setClockDivider(SPI_CLOCK_DIV2);
  LCD.begin(9600);

  //Always keep CS high when not writing.
  digitalWrite(CS, HIGH);
  digitalWrite(CS2, HIGH);

  //Reset the YM2612. Power cycle is insufficient.
  digitalWrite(RESET, LOW);
  delay(100);
  digitalWrite(RESET, HIGH); //Active low
  delay(100);

  lcdinit();

  /* This block , if it still works, will set the YM2612
   to the default Piano voice from sega2.doc
   
   reg(0x22, 0x00); //LFO off
   reg(0x27, 0x00); //Channel 3 mode normal
   reg(0x28, 0x00); //Channel 1 Key Off
   reg(0x28, 0x01); //Channel 2 Key Off
   reg(0x28, 0x02); //Channel 3 Key Off
   reg(0x28, 0x04); //Channel 4 Key Off
   reg(0x28, 0x05); //Channel 5 Key Off
   reg(0x28, 0x06); //Channel 6 Key Off
   reg(0x2B, 0x00); //DAC off
   
   reg(0x30, 0x71); //Detune/Multiply Ch 1 Operator 1 set
   reg(0x31, 0x71); //Detune/Multiply Ch 2 Operator 1 set
   reg(0x32, 0x71); //Detune/Multiply Ch 3 Operator 1 set
   reg2(0x30, 0x71); //Detune/Multiply Ch 4 Operator 1 set
   reg2(0x31, 0x71); //Detune/Multiply Ch 5 Operator 1 set
   reg2(0x32, 0x71); //Detune/Multiply Ch 6 Operator 1 set
   
   reg(0x34, 0x0D); //Detune/Multiply Ch 1 Operator 2 set
   reg(0x35, 0x0D); //Detune/Multiply Ch 2 Operator 2 set
   reg(0x36, 0x0D); //Detune/Multiply Ch 3 Operator 2 set
   reg2(0x34, 0x0D); //Detune/Multiply Ch 4 Operator 2 set
   reg2(0x35, 0x0D); //Detune/Multiply Ch 5 Operator 2 set
   reg2(0x36, 0x0D); //Detune/Multiply Ch 6 Operator 2 set
   
   reg(0x38, 0x33); //Detune/Multiply Ch 1 Operator 3 set
   reg(0x39, 0x33); //Detune/Multiply Ch 2 Operator 3 set
   reg(0x3A, 0x33); //Detune/Multiply Ch 3 Operator 3 set
   reg2(0x38, 0x33); //Detune/Multiply Ch 4 Operator 3 set
   reg2(0x39, 0x33); //Detune/Multiply Ch 5 Operator 3 set
   reg2(0x3A, 0x33); //Detune/Multiply Ch 6 Operator 3 set
   
   reg(0x3C, 0x01); //Detune/Multiply Ch 1 Operator 4 set
   reg(0x3D, 0x01); //Detune/Multiply Ch 2 Operator 4 set
   reg(0x3E, 0x01); //Detune/Multiply Ch 3 Operator 4 set
   reg2(0x3C, 0x01); //Detune/Multiply Ch 4 Operator 4 set
   reg2(0x3D, 0x01); //Detune/Multiply Ch 5 Operator 4 set
   reg2(0x3E, 0x01); //Detune/Multiply Ch 6 Operator 4 set
   
   reg(0x40, 0x23); //Total Level Ch 1 Operator 1 set
   reg(0x41, 0x23); //Total Level Ch 2 Operator 1 set
   reg(0x42, 0x23); //Total Level Ch 3 Operator 1 set
   reg2(0x40, 0x23); //Total Level Ch 4 Operator 1 set
   reg2(0x41, 0x23); //Total Level Ch 5 Operator 1 set
   reg2(0x42, 0x23); //Total Level Ch 6 Operator 1 set
   
   reg(0x44, 0x2D); //Total Level Ch 1 Operator 2 set
   reg(0x45, 0x2D); //Total Level Ch 2 Operator 2 set
   reg(0x46, 0x2D); //Total Level Ch 3 Operator 2 set
   reg2(0x44, 0x2D); //Total Level Ch 4 Operator 2 set
   reg2(0x45, 0x2D); //Total Level Ch 5 Operator 2 set
   reg2(0x46, 0x2D); //Total Level Ch 6 Operator 2 set
   
   reg(0x48, 0x26); //Total Level Ch 1 Operator 3 set
   reg(0x49, 0x26); //Total Level Ch 2 Operator 3 set
   reg(0x4A, 0x26); //Total Level Ch 3 Operator 3 set
   reg2(0x48, 0x26); //Total Level Ch 4 Operator 3 set
   reg2(0x49, 0x26); //Total Level Ch 5 Operator 3 set
   reg2(0x4A, 0x26); //Total Level Ch 6 Operator 3 set
   
   reg(0x4C, 0x00); //Total Level Ch 1 Operator 4 set
   reg(0x4D, 0x00); //Total Level Ch 2 Operator 4 set
   reg(0x4E, 0x00); //Total Level Ch 3 Operator 4 set
   reg2(0x4C, 0x00); //Total Level Ch 4 Operator 4 set
   reg2(0x4D, 0x00); //Total Level Ch 5 Operator 4 set
   reg2(0x4E, 0x00); //Total Level Ch 6 Operator 4 set
   
   reg(0x50, 0x5F); //Attack rate (keyon) Ch 1 Operator 1 set
   reg(0x51, 0x5F); //Attack rate (keyon) Ch 2 Operator 1 set
   reg(0x52, 0x5F); //Attack rate (keyon) Ch 3 Operator 1 set
   reg2(0x50, 0x5F); //Attack rate (keyon) Ch 4 Operator 1 set
   reg2(0x51, 0x5F); //Attack rate (keyon) Ch 5 Operator 1 set
   reg2(0x52, 0x5F); //Attack rate (keyon) Ch 6 Operator 1 set
   
   reg(0x54, 0x5F); //Attack rate (keyon) Ch 1 Operator 2 set
   reg(0x55, 0x5F); //Attack rate (keyon) Ch 2 Operator 2 set
   reg(0x56, 0x5F); //Attack rate (keyon) Ch 3 Operator 2 set
   reg2(0x54, 0x5F); //Attack rate (keyon) Ch 4 Operator 2 set
   reg2(0x55, 0x5F); //Attack rate (keyon) Ch 5 Operator 2 set
   reg2(0x56, 0x5F); //Attack rate (keyon) Ch 6 Operator 2 set
   
   reg(0x58, 0x5F); //Attack rate (keyon) Ch 1 Operator 3 set
   reg(0x59, 0x5F); //Attack rate (keyon) Ch 2 Operator 3 set
   reg(0x5A, 0x5F); //Attack rate (keyon) Ch 3 Operator 3 set
   reg2(0x58, 0x5F); //Attack rate (keyon) Ch 4 Operator 3 set
   reg2(0x59, 0x5F); //Attack rate (keyon) Ch 5 Operator 3 set
   reg2(0x5A, 0x5F); //Attack rate (keyon) Ch 6 Operator 3 set
   
   reg(0x5C, 0x94); //Attack rate (keyon) Ch 1 Operator 4 set
   reg(0x5D, 0x94); //Attack rate (keyon) Ch 2 Operator 4 set
   reg(0x5E, 0x94); //Attack rate (keyon) Ch 3 Operator 4 set
   reg2(0x5C, 0x94); //Attack rate (keyon) Ch 4 Operator 4 set
   reg2(0x5D, 0x94); //Attack rate (keyon) Ch 5 Operator 4 set
   reg2(0x5E, 0x94); //Attack rate (keyon) Ch 6 Operator 4 set
   
   reg(0x60, 5); //Decay rate (keyon) Ch 1 Operator 1 set
   reg(0x61, 5); //Decay rate (keyon) Ch 2 Operator 1 set
   reg(0x62, 5); //Decay rate (keyon) Ch 3 Operator 1 set
   reg2(0x60, 5); //Decay rate (keyon) Ch 4 Operator 1 set
   reg2(0x61, 5); //Decay rate (keyon) Ch 5 Operator 1 set
   reg2(0x62, 5); //Decay rate (keyon) Ch 6 Operator 1 set
   
   reg(0x64, 5); //Decay rate (keyon) Ch 1 Operator 2 set
   reg(0x65, 5); //Decay rate (keyon) Ch 2 Operator 2 set
   reg(0x66, 5); //Decay rate (keyon) Ch 3 Operator 2 set
   reg2(0x64, 5); //Decay rate (keyon) Ch 4 Operator 2 set
   reg2(0x65, 5); //Decay rate (keyon) Ch 5 Operator 2 set
   reg2(0x66, 5); //Decay rate (keyon) Ch 6 Operator 2 set
   
   reg(0x68, 5); //Decay rate (keyon) Ch 1 Operator 3 set
   reg(0x69, 5); //Decay rate (keyon) Ch 2 Operator 3 set
   reg(0x6A, 5); //Decay rate (keyon) Ch 3 Operator 3 set
   reg2(0x68, 5); //Decay rate (keyon) Ch 4 Operator 3 set
   reg2(0x69, 5); //Decay rate (keyon) Ch 5 Operator 3 set
   reg2(0x6A, 5); //Decay rate (keyon) Ch 6 Operator 3 set
   
   reg(0x6C, 7); //Decay rate (keyon) Ch 1 Operator 4 set
   reg(0x6D, 7); //Decay rate (keyon) Ch 2 Operator 4 set
   reg(0x6E, 7); //Decay rate (keyon) Ch 3 Operator 4 set
   reg2(0x6C, 7); //Decay rate (keyon) Ch 4 Operator 4 set
   reg2(0x6D, 7); //Decay rate (keyon) Ch 5 Operator 4 set
   reg2(0x6E, 7); //Decay rate (keyon) Ch 6 Operator 4 set
   
   reg(0x70, 2); //Secondary decay (keyon) Ch 1 Operator 1 set
   reg(0x71, 2); //Secondary decay (keyon) Ch 2 Operator 1 set
   reg(0x72, 2); //Secondary decay (keyon) Ch 3 Operator 1 set
   reg2(0x70, 2); //Secondary decay (keyon) Ch 4 Operator 1 set
   reg2(0x71, 2); //Secondary decay (keyon) Ch 5 Operator 1 set
   reg2(0x72, 2); //Secondary decay (keyon) Ch 6 Operator 1 set
   
   reg(0x74, 2); //Secondary decay (keyon) Ch 1 Operator 2 set
   reg(0x75, 2); //Secondary decay (keyon) Ch 2 Operator 2 set
   reg(0x76, 2); //Secondary decay (keyon) Ch 3 Operator 2 set
   reg2(0x74, 2); //Secondary decay (keyon) Ch 4 Operator 2 set
   reg2(0x75, 2); //Secondary decay (keyon) Ch 5 Operator 2 set
   reg2(0x76, 2); //Secondary decay (keyon) Ch 6 Operator 2 set
   
   reg(0x78, 2); //Secondary decay (keyon) Ch 1 Operator 3 set
   reg(0x79, 2); //Secondary decay (keyon) Ch 2 Operator 3 set
   reg(0x7A, 2); //Secondary decay (keyon) Ch 3 Operator 3 set
   reg2(0x78, 2); //Secondary decay (keyon) Ch 4 Operator 3 set
   reg2(0x79, 2); //Secondary decay (keyon) Ch 5 Operator 3 set
   reg2(0x7A, 2); //Secondary decay (keyon) Ch 6 Operator 3 set
   
   reg(0x7C, 2); //Secondary decay (keyon) Ch 1 Operator 4 set
   reg(0x7D, 2); //Secondary decay (keyon) Ch 2 Operator 4 set
   reg(0x7E, 2); //Secondary decay (keyon) Ch 3 Operator 4 set
   reg2(0x7C, 2); //Secondary decay (keyon) Ch 4 Operator 4 set
   reg2(0x7D, 2); //Secondary decay (keyon) Ch 5 Operator 4 set
   reg2(0x7E, 2); //Secondary decay (keyon) Ch 6 Operator 4 set
   
   reg(0x80, 0x11); //Secondary amplitude/release rate Ch 1 Oper 1 set
   reg(0x81, 0x11); //Secondary amplitude/release rate Ch 2 Oper 1 set
   reg(0x82, 0x11); //Secondary amplitude/release rate Ch 3 Oper 1 set
   reg2(0x80, 0x11); //Secondary amplitude/release rate Ch 4 Oper 1 set
   reg2(0x81, 0x11); //Secondary amplitude/release rate Ch 5 Oper 1 set
   reg2(0x82, 0x11); //Secondary amplitude/release rate Ch 6 Oper 1 set
   
   reg(0x84, 0x11); //Secondary amplitude/release rate Ch 1 Oper 2 set
   reg(0x85, 0x11); //Secondary amplitude/release rate Ch 2 Oper 2 set
   reg(0x86, 0x11); //Secondary amplitude/release rate Ch 3 Oper 2 set
   reg2(0x84, 0x11); //Secondary amplitude/release rate Ch 4 Oper 2 set
   reg2(0x85, 0x11); //Secondary amplitude/release rate Ch 5 Oper 2 set
   reg2(0x86, 0x11); //Secondary amplitude/release rate Ch 6 Oper 2 set
   
   reg(0x88, 0x11); //Secondary amplitude/release rate Ch 1 Oper 3 set
   reg(0x89, 0x11); //Secondary amplitude/release rate Ch 2 Oper 3 set
   reg(0x8A, 0x11); //Secondary amplitude/release rate Ch 3 Oper 3 set
   reg2(0x88, 0x11); //Secondary amplitude/release rate Ch 4 Oper 3 set
   reg2(0x89, 0x11); //Secondary amplitude/release rate Ch 5 Oper 3 set
   reg2(0x8A, 0x11); //Secondary amplitude/release rate Ch 6 Oper 3 set
   
   reg(0x8C, 0xA6); //Secondary amplitude/release rate Ch 1 Oper 4 set
   reg(0x8D, 0xA6); //Secondary amplitude/release rate Ch 2 Oper 4 set
   reg(0x8E, 0xA6); //Secondary amplitude/release rate Ch 3 Oper 4 set
   reg2(0x8C, 0xA6); //Secondary amplitude/release rate Ch 4 Oper 4 set
   reg2(0x8D, 0xA6); //Secondary amplitude/release rate Ch 5 Oper 4 set
   reg2(0x8E, 0xA6); //Secondary amplitude/release rate Ch 6 Oper 4 set
   
   reg(0x90, 0); //Sega doesn't know what this does
   reg(0x94, 0); //Sega doesn't know what this does
   reg(0x98, 0); //Sega doesn't know what this does
   reg(0x9C, 0); //Sega doesn't know what this does
   reg2(0x90, 0); //Sega doesn't know what this does
   reg2(0x94, 0); //Sega doesn't know what this does
   reg2(0x98, 0); //Sega doesn't know what this does
   reg2(0x9C, 0); //Sega doesn't know what this does
   
   reg(0xB0, 0x32); //Feedback/algorithm select, Ch 1
   reg(0xB1, 0x33); //Feedback/algorithm select, Ch 2
   reg(0xB2, 0x34); //Feedback/algorithm select, Ch 3
   reg2(0xB0, 0x35); //Feedback/algorithm select, Ch 4
   reg2(0xB1, 0x37); //Feedback/algorithm select, Ch 5
   reg2(0xB2, 0x31); //Feedback/algorithm select, Ch 6
   
   reg(0xB4, 0xC0); //Both speakers on CH1
   reg(0xB5, 0xC0); //Both speakers on CH2
   reg(0xB6, 0xC0); //Both speakers on CH3
   reg2(0xB4, 0xC0); //Both speakers on CH4
   reg2(0xB5, 0xC0); //Both speakers on CH5
   reg2(0xB6, 0xC0); //Both speakers on CH6
   
   reg(0x28, 0x00); //Key off
   reg(0x28, 0x01); //Key off
   reg(0x28, 0x02); //Key off
   reg(0x28, 0x04); //Key off
   reg(0x28, 0x05); //Key off
   reg(0x28, 0x06); //Key off
  /* Now we are ready to set the frequency and play a note. */

  selectinst(0, CH1);
  selectinst(0, CH2);
  selectinst(0, CH3);
  selectinst(0, CH4);
  selectinst(0, CH5);
  selectinst(0, CH6);
  selectinst(0, CH7);
  selectinst(0, CH8);
  selectinst(0, CH9);
  selectinst(0, CH10);
  selectinst(0, CH11);
  selectinst(0, CH12);

  //vgmplay();
  //flashEEPROM();

  LCD.print("Welcome to");
  lcdline2();
  LCD.print("YM2612 synth");
}

void HandleNoteOff(byte channel, byte note, byte velocity)
{
  if (MODE == MODE_POLY) keyUp(CH1, note);
  else
    if(channel == 1) keyUp(CH1, note);
    else
      if(channel == 2) keyUp(CH2, note);
      else
        if(channel == 3) keyUp(CH3, note);
        else
          if(channel == 4) keyUp(CH4, note);
          else
            if(channel == 5) keyUp(CH5, note);
            else
              if(channel == 6) keyUp(CH6, note);
              else
                if(channel == 7) keyUp(CH7, note);
                else
                  if(channel == 8) keyUp(CH8, note);
                  else
                    if(channel == 9) keyUp(CH9, note);
                    else
                      if(channel == 10) keyUp(CH10, note);
                      else
                        if(channel == 11) keyUp(CH11, note);
                        else
                          if(channel == 12) keyUp(CH12, note);

}

void HandleNoteOn(byte channel, byte note, byte velocity)
{
  if (velocity > 0)
  {
    //Serial.println(String("Note On:  ch=") + channel + ", note=" + note + ", velocity=" + velocity);
    if (MODE == MODE_POLY) keyDown(CH1, note);
    else
      if(channel == 1) keyDown(CH1, note);
      else
        if(channel == 2) keyDown(CH2, note);
        else
          if(channel == 3) keyDown(CH3, note);
          else
            if(channel == 4) keyDown(CH4, note);
            else
              if(channel == 5) keyDown(CH5, note);
              else
                if(channel == 6) keyDown(CH6, note);
                else
                  if(channel == 7) keyDown(CH7, note);
                  else
                    if(channel == 8) keyDown(CH8, note);
                    else
                      if(channel == 9) keyDown(CH9, note);
                      else
                        if(channel == 10) keyDown(CH10, note);
                        else
                          if(channel == 11) keyDown(CH11, note);
                          else
                            if(channel == 12) keyDown(CH12, note);
  }
  else
  {
    //Serial.println(String("Note Off: ch=") + channel + ", note=" + note);
    if (MODE == MODE_POLY) keyUp(CH1, note);
    else
      if(channel == 1) keyUp(CH1, note);
      else
        if(channel == 2) keyUp(CH2, note);
        else
          if(channel == 3) keyUp(CH3, note);
          else
            if(channel == 4) keyUp(CH4, note);
            else
              if(channel == 5) keyUp(CH5, note);
              else
                if(channel == 6) keyUp(CH6, note);
                else
                  if(channel == 7) keyUp(CH7, note);
                  else
                    if(channel == 8) keyUp(CH8, note);
                    else
                      if(channel == 9) keyUp(CH9, note);
                      else
                        if(channel == 10) keyUp(CH10, note);
                        else
                          if(channel == 11) keyUp(CH11, note);
                          else
                            if(channel == 12) keyUp(CH12, note);
  }

}

/*
 Need to not actually change on getting the signal,
 because if we flip through 30 programs it will set them
 all one by one until it catches up.
 
 instead, should have something that polls and only updates
 sometimes.
 
 TODO: this resets the UI event timer, that might be bad
 */
void HandleProgramChange(byte channel, byte number)
{
  if (PROGRAMCHANGE == 0)
  {
    TIMER = 0; 
  }

  PROGRAMCHANGE = 1;
  progchange[0] = channel;
  progchange[1] = number; 
}
void DoProgramChange(byte channel, byte number)
{
  /*
  lcdinit();
   LCD.print("YM2612 Chan. ");
   LCD.print(String(channel, DEC));
   lcdline2();
   LCD.print("Instrument: #");
   LCD.print(number);
   */

  if ( MODE == MODE_POLY)
  {
    for ( int i=0; i <= 12; i++ )
      selectinst(number, CHANNEL[i]);
  }
  else
    selectinst(number, CHANNEL[channel]);
}

//TODO: fix this it's shit, it doesn't even use the "bend" int
void HandlePitchBend(byte channel, int bend)
{
  byte d2 = MIDI.getData2();
  pitchbend(d2 - 64, CH1);
  pitchbend(d2 - 64, CH2);
  pitchbend(d2 - 64, CH3);
}

void HandleControlChange(byte channel, byte number, byte value)
{
  int d2 = value;
  byte buf, offset;
  byte ch = CHANNEL[channel];


  if (ch <= CH3)
  {
    offset = ch;
  }
  else if (ch <= CH6)
  {
    offset = ch - 4;
  }
  else if (ch <= CH9)
  {
    offset = ch - 7;
  }
  else if (ch <= CH12)
  {
    offset = ch -11;
  }

  switch (number)
  {
  case 21:
    lcdinit();
    LCD.print("ALG set CH");
    LCD.print(String(ch, DEC));
    buf = voice[ch].feedback << 3;
    buf += voice[ch].algorithm = d2;
    lcdline2();
    LCD.print(String(voice[ch].feedback, DEC));
    LCD.print(" ");
    LCD.print(String(voice[ch].algorithm, DEC)); 
    reg3(0xB0 + offset, buf, ch);
    break;
  case 22:
    lcdinit();
    LCD.print("LFO");
    LCD.print(String(d2, DEC));
    LCD.print(" ");

    if (d2 == 0)
    {
      reg3(0x22, 0, ch);
    }
    else
    {
      reg3(0x22, d2 -1 + (1 << 3), ch);
    }

    break;
  case 50:
    lcdinit();
    LCD.print("FB set ");
    LCD.print(String(ch, DEC));
    buf = (voice[ch].feedback = d2 ) << 3;
    buf += voice[ch].algorithm;
    lcdline2();
    LCD.print(String(voice[ch].feedback, DEC));
    LCD.print(" ");
    LCD.print(String(voice[ch].algorithm, DEC)); 
    reg3(0xB0 + offset, buf, ch);
    break;
  case 42: //dt1 set
    lcdinit();
    LCD.print("Set DT1, CH");
    LCD.print(String(channel, DEC));
    lcdline2();
    LCD.print("Oper(s): ");

    for (int i=0; i<4; i++)
    {
      if ((chctl & (1 << i)) > 0)
      {
        LCD.print(String(i + 1, DEC));
        buf = (voice[ch].op[i].dt1 = d2) << 4;
        buf += voice[ch].op[i].mul;
        reg3(0x30 + (i * 4) + offset, buf, ch);
      }
    }
    break;
  case 43: //mul set
    lcdinit();
    LCD.print("Set MUL, CH");
    LCD.print(String(channel, DEC));
    lcdline2();
    LCD.print("Oper(s): ");

    for (int i=0; i<4; i++)
    {
      if ((chctl & (1 << i)) > 0)
      {
        LCD.print(String(i + 1, DEC));
        buf = voice[ch].op[i].dt1 << 4;
        buf += voice[ch].op[i].mul = d2;
        reg3(0x30 + (i * 4) + offset, buf, ch);
      }
    }
    break;
  case 44: //TL set
    //lcdinit();
    //LCD.print("Set TL, CH");
    //LCD.print(String(channel, DEC));
    //lcdline2();
    //LCD.print("Oper(s): ");

    for (int i=0; i<4; i++)
    {
      if ((chctl & (1 << i)) > 0)
      {
        //LCD.print(String(i + 1, DEC));
        buf = voice[ch].op[0].tl = d2;
        reg3(0x40 + (i * 4) + offset, buf, ch);
      }
    }
    break;
  case 45: //attack rate set
    lcdinit();
    LCD.print("Set Atk Rate, CH");
    LCD.print(String(channel, DEC));
    lcdline2();
    LCD.print("Oper(s): ");

    for (int i=0; i<4; i++)
    {
      if ((chctl & (1 << i)) > 0)
      {
        LCD.print(String(i + 1, DEC));
        buf = voice[ch].op[i].rs << 6;
        buf += voice[ch].op[i].ar = d2;
        reg3(0x50 + (i * 4) + offset, buf, ch);
      }
    }
    break;
  case 49: //channel LFO sensitivity set
    lcdinit();
    LCD.print("LFO CH");
    LCD.print(String(channel, DEC));
    lcdline2();

    if (d2 == 0)
    {
      /*          for (int i=0; i<4; i++)
       {
       buf = voice[ch].op[i].d1r;
       reg3(0x60 + (i * 4) + offset, buf, ch);
       LCD.print(String(buf, DEC));
       LCD.print(" ");
       }*/
      reg3(0xB4 + offset, 0xC0, ch);
    }
    else
    {
      reg3(0xB4 + offset, 0xC0 + (d2 << 4), ch);
    }

    break;
  case 59: //LFO toggle
    lcdinit();
    LCD.print("LFO");

    for (int i=0; i<4; i++)
    {
      if ((chctl & (1 << i)) > 0)
      {
        buf = voice[ch].op[i].d1r;
        buf += (voice[ch].op[i].lfo = d2) << 7;

        reg3(0x60 + (i * 4) + offset, buf, ch);
      }
    }
    break;
  case 51:
    if (d2 == 127) chctl |= 1 << 0;
    if (d2 == 0) chctl &= ~(1 << 0);
    break;
  case 52:
    if (d2 == 127) chctl |= 1 << 1;
    if (d2 == 0) chctl &= ~(1 << 1);
    break;
  case 53:
    if (d2 == 127) chctl |= 1 << 2;
    if (d2 == 0) chctl &= ~(1 << 2);
    break;
  case 54:
    if (d2 == 127) chctl |= 1 << 3;
    if (d2 == 0) chctl &= ~(1 << 3);
    break;
  case 58:
    break;
  }  
}

/* This is the main program loop. Keep this as small as possible */
void loop()
{
  //uncomment this line to test note playing and audio output with the wipe function
  //while(1) wipe();

  MIDI.read();

  //UI EVENTS GO HERE
  if (TIMER > 250000)
  {
    if (PROGRAMCHANGE) //detected a MIDI program change
    {
      PROGRAMCHANGE = 0;
      DoProgramChange(progchange[0], progchange[1]);
    }
    
    TIMER = 0;
  }
}








