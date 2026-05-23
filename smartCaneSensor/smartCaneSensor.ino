//www.elegoo.com(Credits to the Arduino Libraries given)
//2016.12.08

// Passive buzzer libraries
#include "pitches.h"
 

// ultrasonic sensor libraries
#include <SR04.h>
#define TRIG_PIN 12
#define ECHO_PIN 11
SR04 sr04 = SR04(ECHO_PIN,TRIG_PIN);
long a;

// notes in the melody:
int melody[] = {
  NOTE_C5, NOTE_D5, NOTE_E5, NOTE_F5, NOTE_G5, NOTE_A5, NOTE_B5, NOTE_C6};
int duration = 500;  // 500 miliseconds
 
void setup() {
   Serial.begin(9600);
   delay(1000);
}

void alertSound(){
  for (int thisNote = 0; thisNote < 8; thisNote++) {
    // pin8 output the voice, every scale is 0.5 sencond
    tone(8, melody[thisNote], duration);
     
    // Output the voice after several minutes
    delay(1000);
  }
}



 
void loop() {  
   a=sr04.Distance();
   Serial.print(a);
   Serial.println("cm");
   delay(1000);

   // if object detection is 5cm away alert the user of an object 
   if(a == 5){
    alertSound();
   }

  // restart after two seconds 
  delay(2000);
}
