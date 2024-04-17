#include <Arduino.h>
#include <SPI.h>
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <hardware/flash.h>
#include <pb_encode.h>
#include <pb_decode.h>

#include "pico/stdlib.h"

#include "ICM42688P.h"
#include "msg/messages.pb.h"
#include "cobs.h"

#define DEBUG

#ifdef DEBUG
#define DEBUG_PRINTF(...) Serial1.printf(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...) // Defines DEBUG_PRINT as an empty macro when DEBUG is not defined
#endif


static constexpr char FIRMWARE_VERSION[] =  "0.1.0";

// LED on when SPI transfer ongoing or LED on when state sampling
// #define LED_ON_SPI_TRANSFER_DEBUG

// Config
static constexpr uint16_t kSampleQueueSize = 128;

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

//SPI clk (not exact - sets to first available lower value prolly?)
static constexpr uint32_t spi_clk_hz = 25000000UL;


//packed IDs
static constexpr uint8_t imu_data_id = 0x01;
static constexpr uint8_t status_id = 0x02;
static constexpr uint8_t device_info_id = 0x03;
static constexpr uint8_t command_id = 0x04;

// send status interval
static constexpr uint32_t status_send_interval_ms = 500;

// state enum
enum State {
  IDLE = 0,
  SAMPLING = 1,
  ERROR = 2
};

enum CommandID {
  HANDSHAKE = 0,
  START_SAMPLING = 1,
  STOP_SAMPLING = 2
};

enum Error{
  NO_ERROR = 0,
  SENSOR_COMMS_ERROR = 1,
};


// freertos queue for data read from icm in interrupt
QueueHandle_t raw_data_q;
volatile uint32_t buff_full_sample_missed_count = 0;

ICM42688P icm(&SPI, cs, spi_clk_hz, int1, int2);
uint64_t sample_count = 0;

ICM42688PAllData imu_all_data;
ICM42688PAllData q_pop_data;

// packet framing
IMUData pb_imu_data = IMUData_init_zero;
pb_ostream_t nanopb_stream;
uint8_t nanopb_buffer[IMUData_size];
uint8_t out_buffer[IMUData_size + 4]; // start 0x00, end 0x00, cobs overhead, ID byte (right after start byte)

//incoming data stuff
uint8_t incoming_buffer[64];
uint8_t incoming_cobs_buffer[64];

//status stuff
Status pb_status_data = Status_init_zero;
pb_ostream_t nanopb_status_stream;
uint8_t nanopb_status_buffer[Status_size];
uint8_t out_status_buffer[Status_size + 4]; // start 0x00, end 0x00, cobs overhead, ID byte (right after start byte)
volatile float last_measured_temp = 0.0;
State global_state = State::IDLE;
Error global_error = Error::NO_ERROR;

bool handshake_complete = false;
uint8_t flash_unique_id[8];


void __time_critical_func(DataReadyInterrupt)(){
  #ifdef LED_ON_SPI_TRANSFER_DEBUG
  digitalWrite(usr_led, HIGH);
  #endif
  imu_all_data = icm.ReadAll();
  //save temperature for status packet
  // TODO: temp is not measured when not sampling, status msg will be wrong
  last_measured_temp = imu_all_data.temp;
  if(xQueueSendFromISR(raw_data_q, &imu_all_data, NULL) != pdTRUE){
    // queue send error - queue full!
    // TODO: missing samples if data rate = 4k
    buff_full_sample_missed_count++;
  }
  #ifdef LED_ON_SPI_TRANSFER_DEBUG
  digitalWrite(usr_led, LOW);
  #endif
}

ICM42688P::OutputDataRate IdxToRate(uint8_t idx){
  switch(idx){
    case 0:
      return ICM42688P::OutputDataRate::RATE_12_5;
    case 1:
      return ICM42688P::OutputDataRate::RATE_25;
    case 2:
      return ICM42688P::OutputDataRate::RATE_50;
    case 3:
      return ICM42688P::OutputDataRate::RATE_100;
    case 4: 
      return ICM42688P::OutputDataRate::RATE_200;
    case 5:
      return ICM42688P::OutputDataRate::RATE_500;
    case 6:
      return ICM42688P::OutputDataRate::RATE_1K;
    case 7:
      return ICM42688P::OutputDataRate::RATE_2K;
    case 8:
      return ICM42688P::OutputDataRate::RATE_4K;
    case 9:
      return ICM42688P::OutputDataRate::RATE_8K;
    case 10:  
      return ICM42688P::OutputDataRate::RATE_16K;
    case 11:
      return ICM42688P::OutputDataRate::RATE_32K;
    default:
      DEBUG_PRINTF("Invalid rate idx\r\n");
      return ICM42688P::OutputDataRate::RATE_12_5; //defaults to lowest if invalid
  }
}

ICM42688P::AccelFullScale IdxToAccelRange(uint8_t idx){
  switch(idx){
    case 0:
      return ICM42688P::AccelFullScale::RANGE_2G;
    case 1:
      return ICM42688P::AccelFullScale::RANGE_4G;
    case 2:
      return ICM42688P::AccelFullScale::RANGE_8G;
    case 3:
      return ICM42688P::AccelFullScale::RANGE_16G;
    default:
      DEBUG_PRINTF("Invalid accel range idx");
      return ICM42688P::AccelFullScale::RANGE_16G; //defaults to highest if invalid
  }
}

ICM42688P::GyroFullScale IdxToGyroRange(uint8_t idx){
  switch(idx){
    case 0:
      return ICM42688P::GyroFullScale::RANGE_15_625DPS;
    case 1:
      return ICM42688P::GyroFullScale::RANGE_31_25DPS;
    case 2:
      return ICM42688P::GyroFullScale::RANGE_62_5DPS;
    case 3:
      return ICM42688P::GyroFullScale::RANGE_125DPS;
    case 4:
      return ICM42688P::GyroFullScale::RANGE_250DPS;
    case 5:
      return ICM42688P::GyroFullScale::RANGE_500DPS;
    case 6:
      return ICM42688P::GyroFullScale::RANGE_1000DPS;
    case 7:
      return ICM42688P::GyroFullScale::RANGE_2000DPS;
    default:
      DEBUG_PRINTF("Invalid gyro range idx\r\n");
      return ICM42688P::GyroFullScale::RANGE_2000DPS; //defaults to highest if invalid
  }
}

// basic ICM setup (prepared for sampling)
void SetupICM(){
  //set spi drive strength on icm (6-18ns) to reduce overshoot of MISO
  icm.SetSpiDriveConfigBits(0b100);
  // switch to external clock
  icm.StartClockGen();
  icm.setClockSourceExtInt2();
  delay(100);
  
  // setup data ready interrupt
  icm.SetIntPulsesShort();
  icm.IntAsyncReset();
  icm.EnableDataReadyInt1();
  icm.SetInt1PushPullActiveHighPulsed();
}

// start sampling at specified settings. To change settings, stop sampling and use this function again
void StartSampling(ICM42688P::OutputDataRate rate, ICM42688P::AccelFullScale accel_range, ICM42688P::GyroFullScale gyro_range, FilterConfig filter_cfg){
  //todo: quickfix for now (rate not hot changing correctly)
  icm.SoftReset();
  SetupICM();
  // ...
  icm.SetAccelSampleRate(rate);
  icm.SetGyroSampleRate(rate);
  icm.setAccelFullScale(accel_range);
  icm.setGyroFullScale(gyro_range);
  icm.SetAccelFilterBandwidth(filter_cfg);
  icm.SetAccelModeLn();
  delay(10);
  icm.SetGyroModeLn();
  delay(10);
  //reset missed samples counter
  buff_full_sample_missed_count = 0;
  //attach interrupt on data ready
  attachInterrupt(int1, DataReadyInterrupt, RISING);
  global_state = State::SAMPLING;
}

// stop sampling
void StopSampling(){
  icm.SetAccelModeOff();
  icm.SetGyroModeOff();
  // stop data ready interrupt
  detachInterrupt(int1);
  
  global_state = State::IDLE;
}

void SendHandshake() {
  DeviceInfo pb_device_info = DeviceInfo_init_zero;
  strncpy((char*)pb_device_info.unique_id, (char*)flash_unique_id, sizeof(pb_device_info.unique_id));
  strncpy((char*)pb_device_info.firmware, FIRMWARE_VERSION, sizeof(pb_device_info.firmware) - 1);
  pb_device_info.firmware[sizeof(FIRMWARE_VERSION)] = '\0';
  uint8_t pb_buffer[DeviceInfo_size];
  pb_ostream_t stream = pb_ostream_from_buffer(pb_buffer, sizeof(pb_buffer));
  pb_encode(&stream, DeviceInfo_fields, &pb_device_info);
  size_t pb_size = stream.bytes_written;

  // COBS
  uint8_t out_buffer[DeviceInfo_size + 4];
  cobs_encode_result cobs_result = cobs_encode(out_buffer + 2, sizeof(out_buffer) - 3, pb_buffer, pb_size);
  size_t out_packet_len = cobs_result.out_len + 3;
  out_buffer[0] = 0x00; //start byte
  out_buffer[1] = device_info_id; //id byte
  out_buffer[out_packet_len - 1] = 0x00; //end byte
  Serial.write(out_buffer, out_packet_len);
  handshake_complete = true;
  DEBUG_PRINTF("Handshake sent\r\n");
}

void HandleCommand(Command cmd){
  // id = 0x04;
  // message Command {
  // uint32 state = 1 [(nanopb).int_size = IS_8];
  // uint32 filter_config = 2 [(nanopb).int_size = IS_8];
  // uint32 data_rate = 3 [(nanopb).int_size = IS_8];
  // uint32 acc_range = 4 [(nanopb).int_size = IS_8];
  // uint32 gyro_range = 5 [(nanopb).int_size = IS_8];

  // class State(Enum):
  //     IDLE = 0
  //     SAMPLING = 1
  //     ERROR = 2

  switch(cmd.id){
    case CommandID::STOP_SAMPLING:
      StopSampling();
      break;
    case CommandID::START_SAMPLING:
      StartSampling(IdxToRate(cmd.data_rate), IdxToAccelRange(cmd.acc_range), IdxToGyroRange(cmd.gyro_range), filter_configs[cmd.filter_config]);
      break;
    case CommandID::HANDSHAKE:
      SendHandshake();
      break;
    default:
      break;
  }
  

}

void SendStatus(){
  // if sampling, use last measured temp, else read temperature from icm
  if(global_state == State::SAMPLING){
    pb_status_data.temperature = last_measured_temp;
  }
  else{
    ICM42688PAllData data = icm.ReadAll();
    pb_status_data.temperature = imu_all_data.temp;
  }
  
  pb_status_data.state = global_state;
  pb_status_data.error_code = global_error;
  pb_status_data.missed_samples = buff_full_sample_missed_count;
  
  // nanopb
  nanopb_status_stream = pb_ostream_from_buffer(nanopb_status_buffer, sizeof(nanopb_status_buffer));
  pb_encode(&nanopb_status_stream, Status_fields, &pb_status_data);
  size_t nanopb_size = nanopb_status_stream.bytes_written;

  //COBS
  cobs_encode_result cobs_result = cobs_encode(out_status_buffer + 2, 
      sizeof(out_status_buffer) - 3, nanopb_status_buffer, nanopb_size);
  
  size_t out_packet_len = cobs_result.out_len + 3;

  out_status_buffer[0] = 0x00; //start byte
  //add ID to the front of the packet. ID is never 0, no need to run cobs on it
  out_status_buffer[1] = status_id; //id byte
  out_status_buffer[out_packet_len - 1] = 0x00; //end byte

  // Send
  Serial.write(out_status_buffer, out_packet_len);

}


void SendIMUData() {
  if(uxQueueMessagesWaiting(raw_data_q) > 0){
    xQueueReceive(raw_data_q, &q_pop_data, 0);
  } else {
    return;
  }

  pb_imu_data.count = sample_count++;
  pb_imu_data.acc_x = q_pop_data.accel_x;
  pb_imu_data.acc_y = q_pop_data.accel_y;
  pb_imu_data.acc_z = q_pop_data.accel_z;
  pb_imu_data.gyro_x = q_pop_data.gyro_x;
  pb_imu_data.gyro_y = q_pop_data.gyro_y;
  pb_imu_data.gyro_z = q_pop_data.gyro_z;

  // nanopb
  nanopb_stream = pb_ostream_from_buffer(nanopb_buffer, sizeof(nanopb_buffer));
  pb_encode(&nanopb_stream, IMUData_fields, &pb_imu_data);
  size_t nanopb_size = nanopb_stream.bytes_written;


  // COBS
  cobs_encode_result cobs_result = cobs_encode(out_buffer + 2, 
      sizeof(out_buffer) - 3, nanopb_buffer, nanopb_size);
  
  size_t out_packet_len = cobs_result.out_len + 3;
      
  out_buffer[0] = 0x00; //start byte
  //add ID to the front of the packet. ID is never 0, no need to run cobs on it
  out_buffer[1] = imu_data_id; //id byte
  out_buffer[out_packet_len - 1] = 0x00; //end byte

  // Send
  Serial.write(out_buffer, out_packet_len);
}


#define BUF_SIZE 64

void ParseIncoming() {
  static size_t buf_index = 0;

  while (Serial.available()) {
    // digitalWrite(usr_led, HIGH);
    uint8_t byte = Serial.read();

    if (byte == 0x00 && buf_index == 0) { //start byte 0x00
      // digitalWrite(usr_led, HIGH);
      // start byte detected, increment buffer index
      buf_index++;
      return;
    }
    if(byte == 0x00 && buf_index == 1){
      // digitalWrite(usr_led, HIGH);
      // two 0x00 in a row. first was incorrectly interpreted as start byte but was actually end byte. This byte is now start, reset index to 1
      // tldr: if multiple 0x00 in a row, last one is taken as start byte
      buf_index = 0;
      return;
    }
    if(byte == 0x00 && buf_index > 1){ //end byte 0x00
      // digitalWrite(usr_led, HIGH);
      // end byte detected, process the buffer
      uint8_t packet_id = incoming_buffer[1];
      if(packet_id == 0x04){
        // digitalWrite(usr_led, HIGH);
      }
      //decode COBS
      cobs_decode_result result = cobs_decode(incoming_cobs_buffer, sizeof(incoming_cobs_buffer), incoming_buffer+2, buf_index-2);
      if(result.status == COBS_DECODE_OK){
        // digitalWrite(usr_led, HIGH);
        // Use nanopb to decode the message
        pb_istream_t stream = pb_istream_from_buffer(incoming_cobs_buffer, result.out_len);
        //switch to decode different packet types (not needed for now, only command is received)
        // digitalWrite(usr_led, HIGH);
        switch(packet_id){
          case command_id:{ //namespace brackets to be able to initialize msg
            // digitalWrite(usr_led, HIGH);
            Command msg = Command_init_zero;
            if (pb_decode(&stream, Command_fields, &msg)) {
              // Handle the decoded message
              HandleCommand(msg);
              // digitalWrite(usr_led, HIGH);
            }
            else {
              // Handle nanopb the error
            }
            }break;
          default:
            return;
            break;
        }
      }
      else {
        // Handle COBS the error
      }


      buf_index = 0; //reset buffer index
      return;
    }
    else{ // normal bytes in between start and end
      if(buf_index < BUF_SIZE){
        //add to buffer and increment index
        incoming_buffer[buf_index++] = byte;
      } else {
        // buffer full ERROR
        while(1);
        return;
      }
    }

  }
}

void setup() {
  #ifdef DEBUG
  icm.RegisterDebugSerial(&Serial1);
  #endif
  // TODO: disable interrupts before this
  flash_get_unique_id(flash_unique_id);
  // set_sys_clock_khz(270000, true);

  raw_data_q = xQueueCreate(kSampleQueueSize, sizeof(ICM42688PAllData));

  //set INT1 as input for data ready interrupt
  pinMode(int1, INPUT);
  pinMode(usr_led, OUTPUT);
  
  delay(3000);
  Serial.begin();
  #ifdef DEBUG
  Serial1.begin(115200);
  #endif
  DEBUG_PRINTF("PicoQuake boot ok!");
  
  DEBUG_PRINTF("ID read OK\r\n");

  //print firmware version
  DEBUG_PRINTF("Firmware version: %s\r\n", FIRMWARE_VERSION);

  //print uid
  Serial1.print("UID: ");
  for(int i = 0; i < 8; i++){
    Serial1.print(flash_unique_id[i], HEX);
  }
  DEBUG_PRINTF("\r\n");

  SPI.setRX(spi_miso);
  SPI.setTX(spi_mosi);
  SPI.setSCK(spi_sck);
  // // SPI.setCS(cs);
  SPI.begin();

  //begin ICM42688P
  if(!icm.Begin()){
    global_error = Error::SENSOR_COMMS_ERROR;
    DEBUG_PRINTF("ICM42688P comms error! WHOAMI wrong!\r\n");
    while(1){
      //send error status in loop, do nothing else
      digitalWrite(usr_led, HIGH);
      SendStatus();
      delay(250);
      digitalWrite(usr_led, LOW);
      delay(250);

    }
  }

  SetupICM();
  DEBUG_PRINTF("ICM42688P setup OK!\r\n");

  
  // StartSampling(ICM42688P::OutputDataRate::RATE_4K, ICM42688P::AccelFullScale::RANGE_16G, ICM42688P::GyroFullScale::RANGE_2000DPS, filter_config::f_126hz);

  //setup interrupt on INT1 pin
  // attachInterrupt(int1, DataReadyInterrupt, RISING);

  digitalWrite(usr_led, HIGH);
  delay(100);
  digitalWrite(usr_led, LOW);
  delay(500);
  DEBUG_PRINTF("Ready!\r\n");

}

void loop() {
  static uint32_t last_status_send_time = 0;
  ParseIncoming();
  if (true) {
    if(millis() - last_status_send_time > status_send_interval_ms){
      SendStatus();
      last_status_send_time = millis();
    }
  }

  // turn on LED when State::SAMPLING unless debug LED mode defined (on while SPI transfer)
  #ifndef LED_ON_SPI_TRANSFER_DEBUG
  static State last_state = State::IDLE;
  if(global_state == State::SAMPLING && last_state == State::IDLE){
    last_state = State::SAMPLING;
    digitalWrite(usr_led, HIGH);
  }
  else if(global_state == State::IDLE && last_state == State::SAMPLING){
    last_state = State::IDLE;
    digitalWrite(usr_led, LOW);
  }
  #endif
}

void setup1() {
  delay(6000);
}


void loop1() {
  SendIMUData();
}
