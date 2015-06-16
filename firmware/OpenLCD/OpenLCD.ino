/*
 OpenLCD is an LCD with serial/i2c/spi interfaces.
 By: Nathan Seidle
 SparkFun Electronics
 Date: February 13th, 2015
 License: This code is public domain but you buy me a beer if you use this and we meet someday (Beerware license).

 OpenLCD gives the user multiple interfaces (serial, I2C, and SPI) to control an LCD. SerLCD was the original
 serial LCD from SparkFun that ran on the PIC 16F88 with only a serial interface and limited feature set.
 This is an updated serial LCD.
 
 8MHz Pro Mini

 Backlight levels from original datasheet are wrong. Setting of 22 is 76%. See google doc

 Todo:
 -Check for size jumper
 Check how splash screen works on 16 vs 20 width displays
 -Display message when resetting baud rate
 -Display message when changing baud rate
 -Add additional baud rates
 -Document support for 1 line LCDs
 -Add support for custom I2C addresses. This might be a third tier command in order to maintain backwards compatibility
 Can we shut down/sleep while we wait for incoming things?
 -Add watchdog so that we never freeze/fail
 -Create and document support for re_init command: 124 then 8. Does SerLCD v2 have a clear or reset everything command? It should. Document it.
 -Emergency reset to 9600bps
 -Add PWM software support for blue backlight control on pin 8
 Test blue backlight control
 Test WDT fail
 -Test low level scrolling and cursor commands
 -Test cursor move left/right, on edges
 -Test emergency reset
 Current measurements
 Create docs for LCD manufacturer
 Create SPI examples
 Establish and cut down on boot time
 
 Tests:
 -Change LCD width to 20, then back to 16 (124/3, then 124/4) then send 18 characters and check for wrap
 -Enable/Disable splash screen, send 124 then 9 to toggle, then power cycle
 -Change baud rate: 124/12 to go to 4800bps, power cycle, send characters at 4800



*/

#include <Wire.h> //For I2C functions
#include <LiquidCrystalFast.h> //Faster LCD commands. From PJRC https://www.pjrc.com/teensy/td_libs_LiquidCrystal.html
#include <EEPROM.h>  //Brightness, Baud rate, and I2C address are stored in EEPROM
#include "settings.h" //Defines EEPROM locations for user settings
#include <SoftPWM.h> //For PWM the blue backlight. Uses library from https://code.google.com/p/rogue-code/wiki/SoftPWMLibraryDocumentation
#include <avr/wdt.h> //Watchdog to prevent system freeze
#include <avr/sleep.h> //Needed for sleep_mode
#include <avr/power.h> //Needed for powering down perihperals such as the ADC/TWI and Timers

LiquidCrystalFast SerLCD(LCD_RS, LCD_RW, LCD_EN, LCD_D4, LCD_D5, LCD_D6, LCD_D7);

byte characterCount = 0;
char currentFrame[DISPLAY_BUFFER_SIZE]; //Max of 4 x 20 LCD

bool modeCommand = false; //Used to indicate if a command byte has been received
bool modeSetting = false; //Used to indicate if a setting byte has been received
bool modeContrast = false; //First setting mode, then contrast change mode, then the value to change to
bool modeTWI = false; //First setting mode, then TWI change mode, then the value to change to

// Struct for circular data buffer data received over UART, SPI and I2C are all sent into a single buffer
struct dataBuffer
{
  unsigned char data[BUFFER_SIZE];  // THE data buffer
  unsigned int head;  // store new data at this index
  unsigned int tail;  // read oldest data from this index
}
buffer;  // our data buffer is creatively named - buffer

void setup()
{
  wdt_reset(); //Pet the dog
  wdt_disable(); //We don't want the watchdog during init

  //During testing reset everything
  //for(int x = 0 ; x < 200 ; x++)
  //  EEPROM.write(x, 0xFF);
  
  //Used to test contrast
  EEPROM.write(LOCATION_CONTRAST, DEFAULT_CONTRAST);

  setupContrast(); //Set contrast
  setupBacklight(); //Turn on any backlights
  
  delay(10); //The OLED at 3.3V seems to need some time before we init (5V works fine). 1 is too short, 10 works
  
  setupDisplay(); //Initialize the LCD
  setupSplash(); //Read and display the user's splash screen

  setupUART(); //Setup serial, check for emergency reset after the splash is done

  setupSPI(); //Initialize SPI stuff (enable, mode, interrupts)
  setupTWI(); //Initialize I2C stuff (address, interrupt, enable)
//  setupTimer(); //Setup timer to control interval reading from buffer

/*
  //Power down various bits of hardware to lower power usage  
  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();

  //Shut off Timer2, Timer1, ADC
  ADCSRA &= ~(1<<ADEN); //Disable ADC
  ACSR = (1<<ACD); //Disable the analog comparator
  DIDR0 = 0x3F; //Disable digital input buffers on all ADC0-ADC5 pins
  DIDR1 = (1<<AIN1D)|(1<<AIN0D); //Disable digital input buffer on AIN1/0

  power_timer1_disable(); //We are never going to use these pieces of hardware
  power_timer2_disable();
  power_adc_disable();
*/
  //interrupts();  // Turn interrupts on, and les' go
  //wdt_enable(WDTO_250MS); //Unleash the beast

  SerLCD.print("Big long test of text");
}

void loop()
{
  wdt_reset(); //Pet the dog

  //The TWI interrupt will fire whenever it fires and adds incoming I2C characters to the buffer
  //As does the SPI interrupt
  //Serial is the only one that needs special attention
  serialEvent(); //Check the serial buffer for new data

  while(buffer.tail != buffer.head) updateDisplay(); //If there is new data in the buffer, display it!
  
  //Once we've cleared the buffer, go to sleep
  //power_timer0_disable(); //Shut down peripherals we don't need
  //power_spi_disable();
  sleep_mode(); //Stop everything and go to sleep. Wake up if serial character received

  //power_spi_enable(); //After wake up, power up peripherals
  //power_timer0_enable();
}



// updateDisplay(): This beast of a function is called by the main loop
// If the data relates to a commandMode or settingMode will be set accordingly or a command/setting 
// will be executed from this function.
// If the incoming data is just a character it will be displayed
void updateDisplay()
{
  // First we read from the oldest data in the buffer
  unsigned char incoming = buffer.data[buffer.tail];
  buffer.tail = (buffer.tail + 1) % BUFFER_SIZE;  // and update the tail to the next oldest

  //If the last byte received wasn't special
  if (modeCommand == false && modeSetting == false && modeContrast == false && modeTWI == false)
  {
    //Check to see if the incoming byte is special
    if(incoming == SPECIAL_SETTING) modeSetting = true; //SPECIAL_SETTING is 127
    else if(incoming == SPECIAL_COMMAND) modeCommand = true; //SPECIAL_COMMAND is 254
    else if(incoming == 8) //Backspace
    {
      if(characterCount == 0) characterCount = settingLCDwidth * settingLCDlines; //Special edge case

      characterCount--; //Back up

      currentFrame[characterCount] = ' '; //Erase this spot from the buffer
      displayFrameBuffer(); //Display what we've got
    }
    else //Simply display this character to the screen
    {
      SerLCD.write(incoming);

      currentFrame[characterCount++] = incoming; //Record this character to the display buffer
      if(characterCount == settingLCDwidth * settingLCDlines) characterCount = 0; //Wrap condition
    }
  }
  else if (modeSetting == true)
  {
    
    //LCD width and line settings
    if (incoming >= 3 && incoming <= 7) //Ctrl+c to Ctrl+g
    {
      //Convert incoming value down to 0 to 4
      changeLinesWidths(incoming - 3);
    }

    //Software reset
    else if (incoming == 8) //Ctrl+h
    {
      while(1); //Hang out and let the watchdog punish us
    }

    //Enable / disable splash setting
    else if (incoming == 9) //Ctrl+i
    {
      changeSplashEnable();
    }

    //Save current buffer as splash
    else if (incoming == 10) //Ctrl+j
    {
      changeSplashContent();
    }

    //Set baud rate
    else if (incoming >= 11 && incoming <= 23) //Ctrl+k to ctrl+w
    {
      //Convert incoming value down to 0
      changeUARTSpeed(incoming - 11);
    }

    //Set contrast
    else if (incoming == 24) //Ctrl+x
    {
      modeContrast = true;
      //We now grab the next character on the next loop and use it to change the contrast
    }

    //Set TWI address
    else if (incoming == 25) //Ctrl+y
    {
      modeTWI = true;
      //We now grab the next character on the next loop and use it to change the TWI address
    }

    //Control ignore RX on boot
    else if (incoming == 26) //Ctrl+z
    {
      changeIgnore();
    }

    //Clear screen and buffer
    else if (incoming == 45) //'-'
    {
      SerLCD.clear();
      SerLCD.setCursor(0, 0);

      clearFrameBuffer(); //Get rid of all characters in our buffer
    }

    //Backlight Red or standard white
    else if (incoming >= SPECIAL_RED_MIN && incoming <= (SPECIAL_RED_MIN+29))
    {
      byte brightness = map(incoming, SPECIAL_RED_MIN, SPECIAL_RED_MIN+29, 0, 255); //Covert 30 digit value to 255 digits
      changeBLBrightness(RED, brightness);
    }

    //Backlight Green
    else if (incoming >= SPECIAL_GREEN_MIN && incoming <= (SPECIAL_GREEN_MIN+29))
    {
      byte brightness = map(incoming, SPECIAL_GREEN_MIN, SPECIAL_GREEN_MIN+29, 0, 255); //Covert 30 digit value to 255 digits
      changeBLBrightness(GREEN, brightness);
    }

    //Backlight Blue
    else if (incoming >= SPECIAL_BLUE_MIN && incoming <= (SPECIAL_BLUE_MIN+29))
    {
      byte brightness = map(incoming, SPECIAL_BLUE_MIN, SPECIAL_BLUE_MIN+29, 0, 255); //Covert 30 digit value to 255 digits
      changeBLBrightness(BLUE, brightness);
    }
   
    //TODO Do we need a command to move the cursor?
    //Probably not. Leave that for low level command stuff
    
    modeSetting = false;
  }
  else if (modeTWI == true)
  {
    //Custom TWI address
    changeTWIAddress(incoming);
    
    modeTWI = false;
  }
  else if (modeContrast == true)
  {
    //Adjust the contrast
    changeContrast(incoming);
    
    modeContrast = false;
  }
  else if (modeCommand == true) //Deal with lower level commands
  {
    if(incoming & 1<<7) //This is a cursor position command
    {
      incoming &= 0x7F; //Get rid of the leading 1
      
      byte line = 0;
      byte spot = 0;
      if(incoming >= 0 && incoming <= 19)
      {
        spot = incoming;
        line = 0;
      }
      else if (incoming >= 64 && incoming <= 83)
      {
        spot = incoming - 64;
        line = 1;
      }
      else if (incoming >= 20 && incoming <= 39)
      {
        spot = incoming - 20;
        line = 2;
      }
      else if (incoming >= 84 && incoming <= 103)
      {
        spot = incoming - 84;
        line = 3;
      }

      SerLCD.setCursor(spot, line); //(x, y) - Set to X spot on the given line
    }

    else if(incoming & 1<<4) //This is a scroll/shift command
    {
      /*See page 24/25 of the datasheet: https://www.sparkfun.com/datasheets/LCD/HD44780.pdf
      Bit 3: (S/C) 1 = Display shift, 0 = cursor move
      Bit 2: (R/L) 1 = Shift to right, 0 = shift left
      */

      //Check for display shift or cursor shift
      if(incoming & 1<<3) //Display shift
      {
        if(incoming & 1<<2) SerLCD.scrollDisplayRight(); //Go right
        else SerLCD.scrollDisplayLeft(); //Go left
      }
      else //Cursor move
      {
        //Check for right/left cursor move
        if(incoming & 1<<2) //Right shift
        {
          characterCount++; //Move cursor right
          if(characterCount == settingLCDwidth * settingLCDlines) characterCount = 0; //Wrap condition
        }
        else
        {
          if(characterCount == 0) characterCount = settingLCDwidth * settingLCDlines; //Special edge case
          characterCount--; //Move cursor left
        }
        SerLCD.setCursor(characterCount % settingLCDwidth, characterCount / settingLCDwidth); //Move the cursor
      }
    }

    else if(incoming & 1<<3) //This is a cursor or display on/off control command
    {
      /*See page 24 of the datasheet: https://www.sparkfun.com/datasheets/LCD/HD44780.pdf
       
      Bit 3: Always 1 (1<<3)
      Bit 2: 1 = Display on, 0 = display off
      Bit 1: 1 = Cursor displayed (an underline), 0 = cursor not displayed
      Bit 0: 1 = Blinking box displayed, 0 = blinking box not displayed
       
      You can combine bits 1 and 2 to turn on the underline and then blink a box. */

      //Check for blinking box cursor on/off
      if(incoming & 1<<0) SerLCD.blink();
      else SerLCD.noBlink();

      //Check for underline cursor on/off
      if(incoming & 1<<1) SerLCD.cursor();
      else SerLCD.noCursor();

      //Check for display on/off
      if(incoming & 1<<2) SerLCD.display();
      else SerLCD.noDisplay();
    }

    //We ignore the command that could set LCD to 8bit mode
    modeCommand = false;
  }

}

//Flushes all characters from the frame buffer
void clearFrameBuffer()
{
  //Clear the frame buffer
  characterCount = 0;
  for(byte x = 0 ; x < (settingLCDwidth * settingLCDlines) ; x++)
    currentFrame[x] = ' ';
}
  
//Display the LCD buffer and return the cursor to where it was before the system message
void displayFrameBuffer(void)
{
  //Return display to previous buffer
  SerLCD.clear();
  SerLCD.setCursor(0, 0);

  for(byte x = 0 ; x < (settingLCDlines * settingLCDwidth) ; x++)
    SerLCD.write(currentFrame[x]);
  
  //Return the cursor to its original position
  SerLCD.setCursor(characterCount % settingLCDwidth, characterCount / settingLCDwidth);
}