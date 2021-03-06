
#include "LighthouseSensor.h"

//height of the lighthouse from the floor
//mounted on surface of entertainment center
#define LIGHTHOUSE_CENTER_HEIGHT_FROM_FLOOR_MM 940.0d
//mounted on top of TV
//#define LIGHTHOUSE_CENTER_HEIGHT_FROM_FLOOR_MM 1950.0d
//height of the diode sensors from the floor
#define ROBOT_DIODE_HEIGHT_MM 38.0d

//#define LIGHTHOUSE_DEBUG_SIGNAL 1
//#define LIGHTHOUSE_DEBUG_ERRORS 1
#define M_2PI_3 2.094395102393195d
#define M_PI_3 1.047197551196598d

//timings for 48 MHz
//each laser rotates 180 degrees every 400,000 ticks but is only visible for 120 degrees of that sweep
//so the visible portion of the laser sweep starts at 30/180 * 400,000 = 66,667 ticks
#define SWEEP_START_TICKS 66667
//and the duration of the visible portion of the laser sweep is 120/180 * 400,000 = 266,667 ticks
#define SWEEP_DURATION_TICKS 266667
//x axis, OOTX bit 0
#define SYNC_PULSE_J0_MIN 2950
//y axis, OOTX bit 0
#define SYNC_PULSE_K0_MIN 3450
//x axis, OOTX bit 1
#define SYNC_PULSE_J1_MIN 3950
//y axis, OOTX bit 1
#define SYNC_PULSE_K1_MIN 4450
#define NONSYNC_PULSE_J2_MIN 4950

//we need the base station info block struct to be byte-aligned; otherwise it'll be aligned according to the MCU
//we're running on (32 bits for SAMD21) and the data we want from it will be unintelligible; hence these pragmas
#pragma pack(push)
#pragma pack(1)
typedef struct _BaseStationInfoBlock {
  uint16_t fw_version;
  uint32_t id;
  //several of these values are actually 16-bit floating point numbers, but since our platform doesn't have those, we treat them
  //as unsigned integers for the purpose of allocating space and will have to manually convert them later
  uint16_t fcal_0_phase;
  uint16_t fcal_1_phase;
  uint16_t fcal_0_tilt;
  uint16_t fcal_1_tilt;
  uint8_t sys_unlock_count;
  uint8_t hw_version;
  uint16_t fcal_0_curve;
  uint16_t fcal_1_curve;
  //  */
  //the following three values indicate the "up" vector of the lighthouse
  //x axis is right (-) to left (+) from the perspective of the lighthouse
  int8_t accel_dir_x;
  //y axis is down (-) to up (+)
  int8_t accel_dir_y;
  //z axis is back (-) to front (+)
  int8_t accel_dir_z;
  //so for example, a perfectly upright lighthouse would have an accel vector of 0, 127, 0
  //the front faces 0, 0, 127 from the lighthouse internal coordinate system
  //-x , z, y
  uint16_t fcal_0_gibphase;
  uint16_t fcal_1_gibphase;
  uint16_t fcal_0_gibmag;
  uint16_t fcal_1_gibmag;
  uint8_t mode_current;
  uint8_t sys_faults;
} BaseStationInfoBlock;
#pragma pack(pop)

/**
   Convert a 16-bit IEEE floating point number to a 32-bit IEEE floating point number.
*/
float float16ToFloat32(uint16_t half)
{
  union {
    uint32_t u;
    float f;
  } val;
  val.u = (half & 0x7fff) << 13 | (half & 0x8000) << 16;
  if ((half & 0x7c00) != 0x7c00)
    return val.f * 0x1p112;
  val.u |= 0x7f800000;
  return val.f;
}

Lighthouse* currentLighthouse = NULL;
LighthouseSensor* rightSensor = NULL;
LighthouseSensorInput rightSensorInput;
LighthouseSensor* leftSensor = NULL;
LighthouseSensorInput leftSensorInput;

Lighthouse::Lighthouse()
  : rightSensor(&rightSensorInput, 0),
    leftSensor(&leftSensorInput, 1)
{
}

void Lighthouse::start()
{
  if (currentLighthouse != NULL)
    currentLighthouse->stop();
  currentLighthouse = this;
  rightSensor = this->rightSensor;
  leftSensor = this->leftSensor;

  //configure the timing clock we'll use for counting cycles between IR pules
  setupClock();

  connectPortPinsToInterrupts();

  //setup our external interrupt controller
  setupEIC();

  connectInterruptsToTimer();

  setupTimer();
}

void Lighthouse::setupClock()
{
  SYSCTRL->DFLLCTRL.reg =
    SYSCTRL_DFLLCTRL_WAITLOCK |                     //output clock when DFLL is locked
//    SYSCTRL_DFLLCTRL_BPLCKC |                       //bypass coarse lock
//    SYSCTRL_DFLLCTRL_QLDIS |                        //disable quick lock
//    SYSCTRL_DFLLCTRL_CCDIS |                        //disable chill cycle
    SYSCTRL_DFLLCTRL_STABLE |                       //stable frequency mode
    SYSCTRL_DFLLCTRL_MODE |                         //closed-loop mode
    SYSCTRL_DFLLCTRL_ENABLE;
  while (!SYSCTRL->PCLKSR.bit.DFLLRDY);

  //setup the divisor for the GCLK0 clock source generator
  REG_GCLK_GENDIV = GCLK_GENDIV_DIV(0) |                                  //do not divide the input clock (48MHz / 1)
                    GCLK_GENDIV_ID(3);                                    //for GCLK3

  //configure GCLK0 and enable it
  REG_GCLK_GENCTRL =
//    GCLK_GENCTRL_IDC |                                   //50/50 duty cycles; optimization when dividing input clock by an odd number
    GCLK_GENCTRL_GENEN |                                 //enable the clock generator
    GCLK_GENCTRL_SRC_DFLL48M |                           //set the clock source to 48MHz
//    GCLK_GENCTRL_SRC_XOSC |                              //set the clock source to 32MHz
//    GCLK_GENCTRL_SRC_OSC32K |                            //set the clock source to high-accuracy 32KHz clock
//    GCLK_GENCTRL_SRC_FDPLL96M |                          //set the clock source to 48MHz
//    (0x08 << 8) |
    GCLK_GENCTRL_ID(3);                                  //for GCLK3
  while (GCLK->STATUS.bit.SYNCBUSY);

  //setup the clock output to go to the EIC
  REG_GCLK_CLKCTRL = GCLK_CLKCTRL_CLKEN |                                 //enable the clock
                     GCLK_CLKCTRL_GEN_GCLK3 |                             //to send GCLK3
                     GCLK_CLKCTRL_ID_EIC;                                 //to the EIC peripheral

  //setup the clock output to go to EVSYS channel 0
  REG_GCLK_CLKCTRL = GCLK_CLKCTRL_CLKEN |                                 //enable the clock
                     GCLK_CLKCTRL_GEN_GCLK3 |                             //to send GCLK3
                     GCLK_CLKCTRL_ID_EVSYS_0;                             //to EVSYS channel 0

  //setup the clock output to go to EVSYS channel 1
  REG_GCLK_CLKCTRL = GCLK_CLKCTRL_CLKEN |                                 //enable the clock
                     GCLK_CLKCTRL_GEN_GCLK3 |                             //to send GCLK3
                     GCLK_CLKCTRL_ID_EVSYS_1;                             //to EVSYS channel 1

  //setup the clock output to go to the TCC
  REG_GCLK_CLKCTRL = GCLK_CLKCTRL_CLKEN |                                 //enable the clock
                     GCLK_CLKCTRL_GEN_GCLK3 |                             //to send GCLK3
                     GCLK_CLKCTRL_ID_TCC0_TCC1;                           //to TCC0 and TCC1

  //wait for synchronization
  while (GCLK->STATUS.bit.SYNCBUSY);
}

void Lighthouse::connectPortPinsToInterrupts()
{
  //enable the PORT subsystem
  PM->APBBMASK.bit.PORT_ = 1;

  //set port A (group 0), pin 21 (PA21, Tinyduino proto board pin IO7) as an input
  PORT->Group[0].DIRCLR.reg = PORT_PA21;

  //configure PA21
  PORT->Group[0].PINCFG[21].reg =
//    PORT_PINCFG_PULLEN |         //enable pull-down
    PORT_PINCFG_INEN |           //enable input buffering
    PORT_PINCFG_PMUXEN;          //enable pin muxing

  //mux PA21 over to EXTINT5
  PORT->Group[0].PMUX[10].reg = PORT_PMUX_PMUXO(PORT_PMUX_PMUXE_A_Val);

  //set port A (group 0), pin 9 (PA09, Tinyduino proto board pin IO3) as an input
  PORT->Group[0].DIRCLR.reg = PORT_PA09;

  //configure PA09
  PORT->Group[0].PINCFG[9].reg =
//    PORT_PINCFG_PULLEN |         //enable pull-down
    PORT_PINCFG_INEN |           //enable input buffering
    PORT_PINCFG_PMUXEN;          //enable pin muxing

  //mux PA09 over to EXTINT9
  PORT->Group[0].PMUX[4].reg = PORT_PMUX_PMUXO(PORT_PMUX_PMUXE_A_Val);
}

void Lighthouse::setupEIC()
{
  //turn on power to the external interrupt controller (EIC)
  PM->APBAMASK.bit.EIC_ = 1;

  //disable the EIC while we configure it
  EIC->CTRL.bit.ENABLE = 0;
  while (EIC->STATUS.bit.SYNCBUSY);

//  EIC->CONFIG[0].bit.FILTEN5 = 1;
  //detect both rising and falling edges
  EIC->CONFIG[0].bit.SENSE5 = EIC_CONFIG_SENSE5_BOTH_Val;
  //generate interrupts on interrupt #5 when edges are detected
  EIC->EVCTRL.bit.EXTINTEO5 = 1;

//  EIC->CONFIG[1].bit.FILTEN1 = 1;
  //detect both rising and falling edges
  EIC->CONFIG[1].bit.SENSE1 = EIC_CONFIG_SENSE1_BOTH_Val;
  //generate interrupts on interrupt #9 when edges are detected
  EIC->EVCTRL.bit.EXTINTEO9 = 1;

  //enable the EIC
  EIC->CTRL.bit.ENABLE = 1;

  //wait for synchronization
  while (EIC->STATUS.bit.SYNCBUSY);
}

void Lighthouse::connectInterruptsToTimer()
{
  //enable the event subsystem
  PM->APBCMASK.bit.EVSYS_ = 1;

  //input config for diode #0
  REG_EVSYS_CHANNEL = EVSYS_CHANNEL_EDGSEL(3) |                           //detect both rising and falling edge
                      EVSYS_CHANNEL_PATH_SYNCHRONOUS |                    //synchronously
                      EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_EIC_EXTINT_9) |    //from external interrupt 9
                      EVSYS_CHANNEL_CHANNEL(0);                           //to EVSYS channel 0

  //output config for diode #0
  REG_EVSYS_USER = EVSYS_USER_CHANNEL(1) |                                //attach output from channel 0 (n+1)
                   EVSYS_USER_USER(EVSYS_ID_USER_TCC0_MC_0);              //to user (recipient) TCC0, MC0
  while (!EVSYS->CHSTATUS.bit.USRRDY0);

  //input config for diode #1
  REG_EVSYS_CHANNEL = EVSYS_CHANNEL_EDGSEL(3) |                           //detect both rising and falling edge
                      EVSYS_CHANNEL_PATH_SYNCHRONOUS |                    //synchronously
                      EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_EIC_EXTINT_5) |    //from external interrupt 5
                      EVSYS_CHANNEL_CHANNEL(1);                           //to EVSYS channel 1

  //output config for diode #1
  REG_EVSYS_USER = EVSYS_USER_CHANNEL(2) |                                //attach output from channel 1 (n+1)
                   EVSYS_USER_USER(EVSYS_ID_USER_TCC1_MC_0);              //to user (recipient) TCC1, MC0
  while (!EVSYS->CHSTATUS.bit.USRRDY0);
}

void Lighthouse::setupTimer()
{
  //enable the TCC0 subsystem
  PM->APBCMASK.bit.TCC0_ = 1;

  //disable TCC0 while we configure it
  REG_TCC0_CTRLA &= ~TCC_CTRLA_ENABLE;

  //configure TCC0
  REG_TCC0_CTRLA =
    TCC_CTRLA_CPTEN0 |              //place MC0 into capture (not compare) mode
    TCC_CTRLA_PRESCALER_DIV1;       //set timer prescaler to 1 (48MHz)
//    TCC_CTRLA_CPTEN1 |              //place MC1 into capture (not compare) mode
//    TCC_CTRLA_CPTEN3 |              //place MC3 into capture (not compare) mode
//    TCC_CTRLA_CPTEN2 |              //place MC2 into capture (not compare) mode
//    TCC_CTRLA_ALOCK |
//    TCC_CTRLA_RESOLUTION_DITH4 |
//    TCC_CTRLA_PRESCSYNC_GCLK;

  //set the event control register
  REG_TCC0_EVCTRL =
    TCC_EVCTRL_MCEI0;               //when TCC0/MC0 events occur, capture COUNT to CC0
//    TCC_EVCTRL_MCEI3 |             //when MC3 events occur, capture COUNT to CC3
//    TCC_EVCTRL_MCEI2 |             //when MC2 events occur, capture COUNT to CC2
//    TCC_EVCTRL_MCEI1 |              //when MC1 events occur, capture COUNT to CC1
//    TCC_EVCTRL_TCEI1 |             //enable the event 1 input
//    TCC_EVCTRL_TCEI0 |             //enable the event 0 input
//    TCC_EVCTRL_TCINV1 |             //enable the event 1 inverted input
//    TCC_EVCTRL_TCINV0 |             //enable the event 0 inverted input
//    TCC_EVCTRL_CNTEO |
//    TCC_EVCTRL_TRGEO |
//    TCC_EVCTRL_OVFEO |
//    TCC_EVCTRL_CNTSEL_BOUNDARY |
//    TCC_EVCTRL_EVACT1_RETRIGGER |  //retrigger CC1 on event 1 (each time an edge is detected)
//    TCC_EVCTRL_EVACT0_RETRIGGER;   //retrigger CC0 on event 0 (each time an edge is detected)

  //setup our desired interrupts
  REG_TCC0_INTENSET =
    TCC_INTENSET_MC0;               //enable interrupts when a capture occurs on MC0
//    TCC_INTENSET_MC3 |            //enable interrupts when a capture occurs on MC3
//    TCC_INTENSET_MC2 |            //enable interrupts when a capture occurs on MC2
//    TCC_INTENSET_MC1 |            //enable interrupts when a capture occurs on MC1
//    TCC_INTENSET_CNT |            //enable interrupts for every tick of the counter
//    TCC_INTENSET_OVF |            //enable interrupts on overflow
//    TCC_INTENSET_TRG;             //enable interrupts on retrigger

  //connect the interrupt handler for TCC0
  NVIC_SetPriority(TCC0_IRQn, 0);
  NVIC_EnableIRQ(TCC0_IRQn);

  //enable TCC0
  REG_TCC0_CTRLA |= TCC_CTRLA_ENABLE;

  //wait for TCC0 synchronization
  while (TCC0->SYNCBUSY.bit.ENABLE);

  //enable the TCC1 subsystem
  PM->APBCMASK.bit.TCC1_ = 1;
  
  //disable TCC1 while we configure it
  REG_TCC1_CTRLA &= ~TCC_CTRLA_ENABLE;
  
  //configure TCC1
  REG_TCC1_CTRLA =
    TCC_CTRLA_CPTEN0 |              //place TCC1/MC0 into capture (not compare) mode
    TCC_CTRLA_PRESCALER_DIV1;       //set timer prescaler to 1 (48MHz)

  REG_TCC1_EVCTRL =
    TCC_EVCTRL_MCEI0;               //when TCC1/MC0 events occur, capture COUNT to CC0

  REG_TCC1_INTENSET =
    TCC_INTENSET_MC0;               //enable interrupts when a capture occurs on TCC1/MC0

  //connect the interrupt handler for TCC1
  NVIC_SetPriority(TCC1_IRQn, 0);
  NVIC_EnableIRQ(TCC1_IRQn);

  //enable TCC1
  REG_TCC1_CTRLA |= TCC_CTRLA_ENABLE;

  //wait for synchronization
  while (TCC1->SYNCBUSY.bit.ENABLE);
}

void TCC0_Handler()
{
  //capture CC0; required regardless of whether we actually use the value in order to reset the interrupt flag
  unsigned int cc0 = REG_TCC0_CC0;

  //make sure the buffer is not full
  if (rightSensorInput.hitTickWritePtr != rightSensorInput.hitTickReadPtr) {
    *rightSensorInput.hitTickWritePtr = cc0;

    //updating this must be atomic, so check if we're at the end first
    if (rightSensorInput.hitTickWritePtr == rightSensorInput.hitTickEndPtr)
      rightSensorInput.hitTickWritePtr = rightSensorInput.hitTickBuffer;
    else
      rightSensorInput.hitTickWritePtr++;
  }
}

void TCC1_Handler()
{
  //capture CC0; required regardless of whether we actually use the value in order to reset the interrupt flag
  unsigned int cc0 = REG_TCC1_CC0;

  //make sure the buffer is not full
  if (leftSensorInput.hitTickWritePtr != leftSensorInput.hitTickReadPtr) {
    *leftSensorInput.hitTickWritePtr = cc0;

    //updating this must be atomic, so check if we're at the end first
    if (leftSensorInput.hitTickWritePtr == leftSensorInput.hitTickEndPtr)
      leftSensorInput.hitTickWritePtr = leftSensorInput.hitTickBuffer;
    else
      leftSensorInput.hitTickWritePtr++;
  }
}

void Lighthouse::loop()
{
  rightSensor.loop();
  leftSensor.loop();
}

void Lighthouse::recalculate()
{
  unsigned long currentTime = millis();
  
  //update the position of the left sensor
  if (leftSensor.hasLighthouseSignal())
    leftSensor.recalculatePosition();
  else
    leftSensor.estimatePosition(&previousOrientationVector, &orientationVector, currentTime);

  //update the position of the right sensor
  if (rightSensor.hasLighthouseSignal())
    rightSensor.recalculatePosition();
  else
    rightSensor.estimatePosition(&previousOrientationVector, &orientationVector, currentTime);

  //update the combined (average) position to get the overall position of the robot
  unsigned long combinedPositionTimeStamp = max(leftSensor.positionTimeStamp, rightSensor.positionTimeStamp);
  if (positionTimeStamp != combinedPositionTimeStamp) {
#ifdef LIGHTHOUSE_DEBUG_SIGNAL
    SerialUSB.println("Position is up-to-date.");
#endif
    previousPositionVector.set(&positionVector);
    previousPositionTimeStamp = positionTimeStamp;
    
    positionVector.set((leftSensor.positionVector.getX() + rightSensor.positionVector.getX()) / 2.0d,
        (leftSensor.positionVector.getY() + rightSensor.positionVector.getY()) / 2.0d);
    positionTimeStamp = combinedPositionTimeStamp;
  }
  
  //use the updated positions to calculate the new orientation
  if (orientationTimeStamp != combinedPositionTimeStamp) {
    //orientation is already up-to-date
    previousOrientationVector.set(&orientationVector);
    previousOrientationTimeStamp = orientationTimeStamp;
  
    //calculate the current orientation; the orientation vector is just the down direction (0,0,-1) crossed
    //with the vector between the sensors; this calculation simplifies to the following
    orientationVector.set(leftSensor.positionVector.getY() - rightSensor.positionVector.getY(),
        -(leftSensor.positionVector.getX() - rightSensor.positionVector.getX()), 1.0d);
#ifdef LIGHTHOUSE_DEBUG_SIGNAL
    SerialUSB.print("Recalculated orientation: ")
    SerialUSB.println(orientationVector.getOrientation(), 3);
#endif
    orientationTimeStamp = combinedPositionTimeStamp;
  }

  //now we can use the change in orientation to accurately calculate the velocities of each sensor
  leftSensor.recalculateVelocity(&previousOrientationVector, &orientationVector, combinedPositionTimeStamp);
  rightSensor.recalculateVelocity(&previousOrientationVector, &orientationVector, combinedPositionTimeStamp);
}

void Lighthouse::stop()
{
  REG_TCC0_CTRLA &= ~TCC_CTRLA_ENABLE;
  REG_TCC1_CTRLA &= ~TCC_CTRLA_ENABLE;
}

LighthouseSensor::LighthouseSensor(LighthouseSensorInput* sensorInput,
                                   int dn)
  : debugNumber(dn),
    zeroCount(0),
    syncBitCounter(0),
    payloadLength(0),
    payloadReadMask(0),
    readInfoBlockIndex(0),
    readInfoBlockMask(0),
    pendingCycleEdge(SyncRising),
    previousTickCount(0),
    receivedLighthousePosition(false),
    currentCycle(-1)
{
  this->sensorInput = sensorInput;
}

int8_t LighthouseSensor::getAccelDirX() {
  return ((BaseStationInfoBlock*)baseStationInfoBlock)->accel_dir_x;
}

int8_t LighthouseSensor::getAccelDirY() {
  return ((BaseStationInfoBlock*)baseStationInfoBlock)->accel_dir_y;
}

int8_t LighthouseSensor::getAccelDirZ() {
  return ((BaseStationInfoBlock*)baseStationInfoBlock)->accel_dir_z;
}

/**
 * Calculate the orientation and position of the lighthouse relative to the ground plane.
 *
 * From this accelerometer reading, calculate a quaternion that represents the lighthouse rotation in a coordinate system where the
 * x and y axes are parallel to the ground, positive x is to the right from the lighthouse, positive y is forward from the lighthouse,
 * and positive z represents height.
 */
void LighthouseSensor::calculateLighthousePosition()
{
  /*
   * Lighthouse factory calibration data for the lighthouse being used for beta testing and development.
   * 
   * X Rotor Factory Calibration:
   *   Phase (degrees):         1.116435
   *   Tilt (degrees):          0.312331
   *   Curve (degrees):        -0.070105
   *   Gibbous Phase (?):       1.673828
   *   Gibbous Magnitude (?):   0.025238
   *
   * Y Rotor Factory Calibration:
   *   Phase (degrees):         0.568272
   *   Tilt (degrees):         -0.130265
   *   Curve (degrees):         0.133325
   *   Gibbous Phase (?):       0.238892
   *   Gibbous Magnitude (?):  -0.007553
   */

  //capture the factory calibration data for the x rotor
  xRotor.phase = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_0_phase);
  xRotor.tilt = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_0_tilt);
  xRotor.curve = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_0_curve);
  xRotor.gibbousPhase = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_0_gibphase);
  xRotor.gibbousMagnitude = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_0_gibmag);

  //capture the factory calibration data for the y rotor
  yRotor.phase = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_1_phase);
  yRotor.tilt = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_1_tilt);
  yRotor.curve = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_1_curve);
  yRotor.gibbousPhase = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_1_gibphase);
  yRotor.gibbousMagnitude = float16ToFloat32(((BaseStationInfoBlock*)baseStationInfoBlock)->fcal_1_gibmag);

#ifdef LIGHTHOUSE_DEBUG_SIGNAL
  SerialUSB.println("X Rotor Factory Calibration:");
  SerialUSB.println((xRotor.phase / M_PI) * 180.0d, 6);
  SerialUSB.println((xRotor.tilt / M_PI) * 180.0d, 6);
  SerialUSB.println((xRotor.curve / M_PI) * 180.0d, 6);
  SerialUSB.println(xRotor.gibbousPhase, 6);
  SerialUSB.println(xRotor.gibbousMagnitude, 6);
  SerialUSB.println();

  SerialUSB.println("Y Rotor Factory Calibration:");
  SerialUSB.println((yRotor.phase / M_PI) * 180.0d, 6);
  SerialUSB.println((yRotor.tilt / M_PI) * 180.0d, 6);
  SerialUSB.println((yRotor.curve / M_PI) * 180.0d, 6);
  SerialUSB.println(yRotor.gibbousPhase, 6);
  SerialUSB.println(yRotor.gibbousMagnitude, 6);
  SerialUSB.println();
#endif

  //The accelerometer reading from the lighthouse gives us a vector that represents the lighthouse "up" direction in a coordinate system
  //where the x and z axes are parallel to the ground, positive x is to the lighthouse "left", positive z is "forward, and positive y is
  //"up". This means swapping the y and z axes of the accelerometer and flipping the x axis to put them into our global coordinate system.
  KVector3 rotationUnitVector(-getAccelDirX(), getAccelDirZ(), getAccelDirY(), 1.0d);
  //rotationUnitVector.printDebug();

  //now calculate the angle of rotation from the "up" normal in our global coordinate system (0,0,1) to the rotation unit vector
  //this calculation ultimately reduces to the inverse cosine of the z axis of the rotation unit vector
  double angleOfRotation = acos(rotationUnitVector.getZ());
  //SerialUSB.println((angleOfRotation / M_PI) * 180.0d, 2);

  //now cross the "up" vector of the lighthouse with the "up" normal of the global coordinate system to obtain the axis of rotation for
  //our quaternion; this calculation ultimately reduces to the y axis from the rotation unit vector becoming the x axis and the x axis
  //becoming the negative y axis; then obtain the unit vector of the result
  rotationUnitVector.set(rotationUnitVector.getY(), -rotationUnitVector.getX(), 0.0d, 1.0d);

  //now that we have both the axis and angle of rotation, we can calculate our quaternion
  lighthouseOrientation.set(rotationUnitVector.getX(), rotationUnitVector.getY(), rotationUnitVector.getZ(), angleOfRotation);

  //take the forward unit vector in the lighthouse's coordinate system (0,1,0), and un-rotate it to get it into the global coordinate system
  KVector3 lighthouseForwardVector(0.0d, 1.0d, 0.0d);
  lighthouseForwardVector.unrotate(&lighthouseOrientation);
  //KVector3 lighthouseForwardVector(getAccelDirX(), getAccelDirY(), -getAccelDirZ(), 1.0d);
  //lighthouseForwardVector.printDebug();

  //determine the height of the lighthouse from the diode plane
  double lighthouseDistanceFromDiodePlane = LIGHTHOUSE_CENTER_HEIGHT_FROM_FLOOR_MM - ROBOT_DIODE_HEIGHT_MM;

  //now we intersect the "forward" vector from the lighthouse with the diode plane to determine the relative x/y location where it's pointing
  //that location becomes our origin point in our global coordinate system; the lighthouse is considered to be offset from that location
  double t = -lighthouseDistanceFromDiodePlane / lighthouseForwardVector.getZ();
  lighthousePosition.set(-lighthouseForwardVector.getX() * t,
                         -lighthouseForwardVector.getY() * t,
                         lighthouseDistanceFromDiodePlane);
  //lighthousePosition.printDebug();

  receivedLighthousePosition = true;
}

unsigned int calculateDeltaTicks(unsigned int startTicks, unsigned int endTicks)
{
  //calculate the delta between the ticks; they are derived from a 24-bit counter, so there is a weird hoop to jump through when it rolls over
  if (startTicks > endTicks)
    return (0x01000000 - startTicks) + endTicks;
  else
    return endTicks - startTicks;
}

//returns true when the x or y tick counts are updated
void LighthouseSensor::loop()
{
#ifdef LIGHTHOUSE_DEBUG_ERRORS
  //ensure we didn't overflow our buffer; show a warning if so
  if (sensorInput->hitTickWritePtr == sensorInput->hitTickReadPtr) {
    SerialUSB.print(debugNumber);
    SerialUSB.println(" WARNING: Buffer overflow. Potential missed frames.");
  }
#endif

  //this is volatile, so grab it first
  unsigned int* hitTickWritePtr = (unsigned int*)sensorInput->hitTickWritePtr;
  while (true) {
    //we read behind the writer, and updates to our read pointer must be atomic, so check that we're not at the
    //end of the buffer before updating the read pointer to the next value to be read
    unsigned int* nextReadPtr = sensorInput->hitTickReadPtr;
    if (nextReadPtr == sensorInput->hitTickEndPtr)
      nextReadPtr = sensorInput->hitTickBuffer;
    else
      nextReadPtr++;

    //exit when the buffer is empty
    if (nextReadPtr == hitTickWritePtr)
      break;

    //then let the read pointer move forward; must be atomic, hence the temp pointer above until we confirmed we can move forward
    sensorInput->hitTickReadPtr = nextReadPtr;

    //get our next tick count delta
    unsigned int currentTickCount = *nextReadPtr;

    switch (pendingCycleEdge) {
      case SyncFalling:
        processSyncSignal(previousTickCount, currentTickCount);
        break;
      case SweepRising:
        processSweepHit(previousTickCount, currentTickCount);
        break;
      case SweepFalling:
        //this should be the falling edge of the sweep hit; switch to watching for the other position
        currentCycle = (currentCycle + 1) & 0x1;
        pendingCycleEdge = SyncRising;
        break;
      case SyncRising:
        //this should be the rising edge of the sync pulse
        pendingCycleEdge = SyncFalling;
        break;
    }
    previousTickCount = currentTickCount;
  }
}

void LighthouseSensor::processSyncSignal(unsigned int previousTickCount, unsigned int currentTickCount)
{
  //in the mode of waiting for a sweep hit
  unsigned int deltaTicks = calculateDeltaTicks(previousTickCount, currentTickCount);
  //check if this is a sync pulse, the length of which should be greater than SYNC_PULSE_J0_MIN, but less than
  //NONSYNC_PULSE_J2_MIN, since we only have one lighthouse base station; this would need to change if we add
  //a second lighthouse base station
  if (deltaTicks < SYNC_PULSE_J0_MIN || deltaTicks >= NONSYNC_PULSE_J2_MIN) {
    if (currentCycle != -1) {
      //indicate that we lost the signal for the currently expected axis
      cycleData[currentCycle].sweepHitTimeStamp = 0;
      
#ifdef LIGHTHOUSE_DEBUG_ERRORS
      SerialUSB.print(debugNumber);
      SerialUSB.print(" Lost lighthouse signal: ");
      SerialUSB.println(deltaTicks);
#endif
    }
    
    //just stay in "SyncFalling" mode to continue looking for next sync hit
    currentCycle = -1;
    return;
  }

  //found a sync pulse; extract the OOTX bit if we still need the base station info block
  if (!receivedLighthousePosition)
    processOOTXBit(deltaTicks);

  //we got a hit from the sweep; so is the sync pulse X or Y?
  //X is 3000-3499 or 4000-4499; Y is 3500-3999 or 4500-4999
  int foundCycle = (((deltaTicks - SYNC_PULSE_J0_MIN) / 500) & 0x1);
  if (currentCycle != -1 && foundCycle != currentCycle) {
    //we missed a cycle; clear the previous cycle's data since it was skipped
    memset(&cycleData[currentCycle], 0, sizeof(SensorCycleData));

#ifdef LIGHTHOUSE_DEBUG_ERRORS
    SerialUSB.print(debugNumber);
    SerialUSB.print(currentCycle ? " Y" : " X");
    SerialUSB.println(" axis expected sync signal missed.");
#endif

    //indicate that we lost the lighthouse signal for the previous axis, at least for the moment
    cycleData[currentCycle].sweepHitTimeStamp = 0;
  }

  currentCycle = foundCycle;
  cycleData[currentCycle].pendingSyncTickCount = deltaTicks;

  pendingCycleEdge = SweepRising;
}

/**
   Check if we got a hit from the sweep, which starts at SWEEP_START_TICKS after the beginning of the sync pulse.
*/
void LighthouseSensor::processSweepHit(unsigned int previousTicks, unsigned int currentTicks)
{
  unsigned int deltaTicks = calculateDeltaTicks(previousTicks, currentTicks);
  unsigned long sweepTickCount = cycleData[currentCycle].pendingSyncTickCount + deltaTicks - SWEEP_START_TICKS;
  if (sweepTickCount >= SWEEP_DURATION_TICKS) {
    //we missed the sweep pulse; clear this cycle's data since the sweep was missed
    memset(&cycleData[currentCycle], 0, sizeof(SensorCycleData));

#ifdef LIGHTHOUSE_DEBUG_ERRORS
    SerialUSB.print(debugNumber);
    SerialUSB.print(" WARNING: Missed a sweep on ");
    SerialUSB.print(currentCycle ? "Y" : "X");
    SerialUSB.println(" axis after seeing the proper sync pulse.");
#endif

    //go back to watching for the next sync signal
    currentCycle = -1;
    pendingCycleEdge = SyncFalling;
    return;
  }

  cycleData[currentCycle].syncTickCount = cycleData[currentCycle].pendingSyncTickCount;
  cycleData[currentCycle].pendingSyncTickCount = 0;
  cycleData[currentCycle].sweepTickCount = sweepTickCount;
  cycleData[currentCycle].sweepHitTimeStamp = millis();

  pendingCycleEdge = SweepFalling;
}

/**
   Each sync pulse represents either a zero bit (3000-4000 ticks pulse width) or a one bit (4000-5000) of the OOTX
   frame, with a one bit always occurring every 17th pulse to frame the data bits (aka, the "sync bit"), and 17 zero
   bits representing the start of the frame, since 17 zero bits cannot occur in the middle of the data stream due
   to the sync bits.
*/
void LighthouseSensor::processOOTXBit(unsigned int syncDelta)
{
  bool value = syncDelta >= SYNC_PULSE_J1_MIN;
  /*
    #ifdef LIGHTHOUSE_DEBUG_SIGNAL
      SerialUSB.print(debugNumber);
      SerialUSB.print(" Received an OOTX bit. Ticks: ");
      SerialUSB.print(syncDelta);
      SerialUSB.print("   Bit: ");
      SerialUSB.println(value ? 1 : 0);
    #endif
  */
  syncBitCounter++;
  if (!value) {
    zeroCount++;

    if (zeroCount == 17) {
      //found start of OOTX frame

#ifdef LIGHTHOUSE_DEBUG_SIGNAL
      SerialUSB.print(debugNumber);
      SerialUSB.println(" Found the start of the OOTX frame.");
#endif

#ifdef LIGHTHOUSE_DEBUG_ERRORS
      if (payloadReadMask || readInfoBlockMask) {
        SerialUSB.print(debugNumber);
        SerialUSB.println(" WARNING: OOTX frame restarted");
      }
#endif
      //cancel any packet we were previously reading
      readInfoBlockMask = 0;

      zeroCount = 0;
      syncBitCounter = 16;
      payloadReadMask = 0x0080;

      return;
    }
    else if (syncBitCounter == 17 && (payloadReadMask || readInfoBlockMask)) {
      //expecting a sync bit and didn't get it; start over
#ifdef LIGHTHOUSE_DEBUG_ERRORS
      SerialUSB.print(debugNumber);
      SerialUSB.print(" WARNING: Missed a sync bit: ");
      SerialUSB.println(syncDelta);
#endif
      syncBitCounter = 0;

      //cancel anything we were previously reading; wait for the start of another OOTX frame
      payloadReadMask = 0;
      readInfoBlockMask = 0;
      return;
    }
  }
  else {
    zeroCount = 0;
    if (syncBitCounter == 17) {
      //excellent; got a sync bit right where we expected it
      syncBitCounter = 0;
      //now is this the end of the info block
      if (readInfoBlockIndex == BASE_STATION_INFO_BLOCK_SIZE) {
        readInfoBlockIndex = 0;

#ifdef LIGHTHOUSE_DEBUG_SIGNAL
        SerialUSB.print(debugNumber);
        SerialUSB.println(" Got the base station info block.");
#endif

        //now calculation the position and orientation of the lighthouse
        calculateLighthousePosition();
      }
      return;
    }
  }

  if (payloadReadMask) {
    if (value)
      payloadLength |= payloadReadMask;
    else
      payloadLength &= ~payloadReadMask;

    payloadReadMask >>= 1;
    if (payloadReadMask == 0) {
      payloadReadMask = 0x8000;
    }
    else if (payloadReadMask == 0x0080) {
      payloadReadMask = 0;
      if (payloadLength == BASE_STATION_INFO_BLOCK_SIZE) {
#ifdef LIGHTHOUSE_DEBUG_SIGNAL
        SerialUSB.print(debugNumber);
        SerialUSB.println(" Starting to read base station info block.");
#endif
        readInfoBlockIndex = 0;
        readInfoBlockMask = 0x80;
      }
#ifdef LIGHTHOUSE_DEBUG_ERRORS
      else {
        SerialUSB.print(debugNumber);
        SerialUSB.print(" WARNING: Receiving an OOTX frame that is NOT the base station info block of size: ");
        SerialUSB.println(payloadLength, BIN);
      }
#endif
    }
  }
  else if (readInfoBlockMask) {
    if (value)
      baseStationInfoBlock[readInfoBlockIndex] |= readInfoBlockMask;
    else
      baseStationInfoBlock[readInfoBlockIndex] &= ~readInfoBlockMask;

    readInfoBlockMask >>= 1;
    if (readInfoBlockMask == 0) {
      readInfoBlockIndex++;
      if (readInfoBlockIndex < BASE_STATION_INFO_BLOCK_SIZE)
        readInfoBlockMask = 0x80;
    }
  }
}

double tickCountToAngle(double tickCount, RotorFactoryCalibrationData* fcalData)
{
  double angleFromLighthouse = tickCount / ((double)SWEEP_DURATION_TICKS);
  double vectorFromLighthouse = tan(((angleFromLighthouse - 0.5d) * M_2PI_3) - fcalData->phase);
  return vectorFromLighthouse;
}

/*
 * Translates combined x and y tick counts into a vector from the lighthouse to the zippy in the global coordinate system.
 */
void LighthouseSensor::recalculatePosition()
{
  if (!cycleData[0].sweepHitTimeStamp || !cycleData[1].sweepHitTimeStamp) {
    //we have no lighthouse signal
    return;
  }
  
  unsigned long newPositionTimeStamp = max(cycleData[0].sweepHitTimeStamp, cycleData[1].sweepHitTimeStamp);
  if (positionTimeStamp == newPositionTimeStamp) {
    //nothing to do; position is up-to-date
//    SerialUSB.println("Position is up-to-date.");
    return;
  }

  previousPositionVector.set(&positionVector);
  previousPositionTimeStamp = positionTimeStamp;
  
  //Step 1: Calculate the vector from the lighthouse in its reference frame to the diode.
  //start by normalizing the angle on each axis from the lighthouse to be from 0.0 to 1.0
  double angleFromLighthouseX = (((double)cycleData[0].sweepTickCount) / ((double)SWEEP_DURATION_TICKS));
  double angleFromLighthouseZ = (((double)cycleData[1].sweepTickCount) / ((double)SWEEP_DURATION_TICKS));
  //  SerialUSB.println(angleFromLighthouseX, 2);

  //at y=1, we want the x and z coordinates of our direction vector; since the tangent is TAN = O / A, then O = TAN / A; given that
  //our adjacent is 1.0, then the opposite (the length of each leg of the vector from our lighthouse) is simple the TAN of the angle
  //along the x and z axes scaled to an angle from -60 degrees to +60 degrees, which is the field of view of the lighthouse
  //TODO: eventually account for lighthouse factory calibration data
  //  double vectorFromLighthouseX = tan((angleFromLighthouseX - 0.5d) * M_2PI_3);
  //  double vectorFromLighthouseZ = tan((angleFromLighthouseZ - 0.5d) * M_2PI_3);
  double vectorFromLighthouseX = tan(((angleFromLighthouseX - 0.5d) * M_2PI_3) + xRotor.phase);
  double vectorFromLighthouseZ = tan(((angleFromLighthouseZ - 0.5d) * M_2PI_3) + yRotor.phase);

  //Step 2: Convert the vector from the lighthouse in its local coordinate system to our global coordinate system.
  //flip the x axis; it appears that our tick counts get greater from left-to-right when facing the lighthouse; this is contrary to
  //some animations online which illustrate the horizontal beam sweeping from right-to-left
  KVector3 directionFromLighthouse(-vectorFromLighthouseX, 1.0d, vectorFromLighthouseZ);
  directionFromLighthouse.unrotate(&lighthouseOrientation);
  directionFromLighthouse.normalize();
  //  directionFromLighthouse.printDebug();

  //now intersect with the plane of the diodes on the robot; since our diode plane is defined by the normal 0,0,1, and we have a
  //vector which identifies the position of the lighthouse from 0,0,0, the entire formula for our ground-plane intersection reduces
  //to the following
  double t = -lighthousePosition.getZ() / directionFromLighthouse.getZ();

  positionVector.set(lighthousePosition.getX() + (directionFromLighthouse.getX() * t),
      lighthousePosition.getY() + (directionFromLighthouse.getY() * t));
  positionTimeStamp = newPositionTimeStamp;
}

void LighthouseSensor::estimatePosition(KVector2* previousOrientation, KVector2* currentOrientation, unsigned long currentTime)
{
  KVector2 deltaPosition(positionVector.getX() - previousPositionVector.getX(), positionVector.getY() - previousPositionVector.getY());
  deltaPosition.rotate(previousOrientation->angleToVector(currentOrientation));
  
  previousPositionVector.set(&positionVector);
  previousPositionTimeStamp = positionTimeStamp;

  positionVector.add(&deltaPosition);
  positionTimeStamp = currentTime;
}

void LighthouseSensor::recalculateVelocity(KVector2* previousOrientation, KVector2* currentOrientation, unsigned long orientationTimeStamp)
{
  if (!previousPositionTimeStamp || !positionTimeStamp || velocityTimeStamp == positionTimeStamp) {
    //either we don't yet have enough position history to determine velocity or our velocity is up-to-date
    return;
  }

//  SerialUSB.println("Updating velocity.");
  KVector2 deltaPosition(positionVector.getX() - previousPositionVector.getX(), positionVector.getY() - previousPositionVector.getY());
  
  //poor calculation by estimating velocity as the direct distance
  //to properly calculate velocity, we need to calculate the length of the elliptical curve from the previous point to the current point
  double deltaSeconds = ((double)(positionTimeStamp - previousPositionTimeStamp)) / 1000.0d;
  velocity = deltaPosition.getD() / deltaSeconds;
//  SerialUSB.println(positionTimeStamp - previousPositionTimeStamp);
  
  //now determine if it is negative
  if (deltaPosition.dotVector(currentOrientation) < 0.0d)
    velocity = -velocity;
    
  velocityTimeStamp = positionTimeStamp;
}

