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

// selected pitches.h sound fitted toward alerting about a real time alert of an object nearby
int alertMelody[] = {
  NOTE_C6,
  NOTE_G6,
  NOTE_C7,
  NOTE_G6
};

int alertDurations[] = {
  120,
  120,
  180,
  200
};

int lenAlertDuration = sizeof(alertDurations) / sizeof(alertDurations[0]);
 
void setup() {
   Serial.begin(9600);
}

void alertSound(){
  // alert sound focus: on grabbing user's attention as quick as possible
  for(int startPitch = 0; startPitch < lenAlertDuration; ++startPitch){
    tone(8, alertMelody[startPitch], alertDurations[startPitch]);
    delay(alertDurations[startPitch] + 50); 

    delay(20);
  }
}



 
void loop() {  
   a=sr04.Distance();
   Serial.print(a);
   Serial.println("cm");
   delay(200);

   // if object detection is 5cm away alert the user of an object 
   if(a <= 10){
    alertSound();
   }

  // restart after two seconds 
  delay(200);
}
