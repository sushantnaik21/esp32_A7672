#include "board_include.h"

char readBuffer[1024];
uint32_t readLength; 
void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  // GPS serial for GSM
  Serial1.begin(115200, SERIAL_8N1, 12, 13);

  init_at_manager();
}

void loop() {
  // put your main code here, to run repeatedly:
  while (Serial1.available()) {
    // get the new byte:
    readLength = Serial.readBytes(readBuffer, 256);
    if( readLength > 0)
    {
      serial2_receive_cb((uint8_t *) readBuffer,readLength);
      Serial.println(readLength);
    }
    
  }
}
