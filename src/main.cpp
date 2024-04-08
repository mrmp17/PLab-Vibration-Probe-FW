#include <Arduino.h>
#include <SPI.h>
#include <ICM42688P.h>
#include <FreeRTOS.h>
#include <task.h>

//PINS
//ICM42688P connections
static constexpr uint8_t spi_miso = 16;
static constexpr uint8_t spi_mosi = 19;
static constexpr uint8_t spi_sck = 18;
static constexpr uint8_t cs = 17;
static constexpr uint8_t int1 = 28;
static constexpr uint8_t int2 = 27;
//usr led
static constexpr uint8_t usr_led = 4;

//SPI clk
static constexpr uint32_t spi_clk_hz = 1000000;

ICM42688P icm(&SPI, cs, spi_clk_hz, int1, int2);




void DataReadyInterrupt(){
  //todo
}


void setup() {
  delay(5000);
  Serial.begin(115200);
  Serial.println("PLab vibration probe boot ok!");
  //set INT1 as input for data ready interrupt
  pinMode(int1, INPUT);
  pinMode(usr_led, OUTPUT);

  SPI.setRX(spi_miso);
  SPI.setTX(spi_mosi);
  SPI.setSCK(spi_sck);
  // // SPI.setCS(cs);
  SPI.begin();

  //begin ICM42688P
  if(!icm.Begin()){
    Serial.println("ICM42688P not found Stopping here!");
    Serial.println("Rebooting in 5 seconds...");
    vTaskDelay(5000);
    rp2040.reboot();
  }

  icm.SetAccelModeLn();
  icm.SetGyroModeLn();
  icm.SetAccelSampleRate(ICM42688P::AccelOutputDataRate::RATE_25);
  icm.SetGyroSampleRate(ICM42688P::GyroOutputDataRate::RATE_25);
  //
  //setup interrupt on INT1 pin
  // attachInterrupt(int1, DataReadyInterrupt, RISING);



}

void loop() {
  // put your main code here, to run repeatedly:
  while(!icm.CheckDataReady());
  digitalWrite(usr_led, HIGH);
  ICM42688PAllData data = icm.ReadAll();
  Serial.println("#####################");
  Serial.print("X: ");
  Serial.print(data.accel_x);
  Serial.print(" Y: ");
  Serial.print(data.accel_y);
  Serial.print(" Z: ");
  Serial.println(data.accel_z);
  Serial.print("Gyro X: ");
  Serial.print(data.gyro_x);
  Serial.print(" Gyro Y: ");
  Serial.print(data.gyro_y);
  Serial.print(" Gyro Z: ");
  Serial.println(data.gyro_z);
  Serial.print("Temp: ");
  Serial.println(data.temp);
  vTaskDelay(5);
  digitalWrite(usr_led, LOW);






  // digitalWrite(usr_led, HIGH);
  // Serial.println("Hello World ON!");
  // vTaskDelay(1000);
  // digitalWrite(usr_led, LOW);
  // Serial.println("Hello World OFF!");
  // vTaskDelay(1000);
}
