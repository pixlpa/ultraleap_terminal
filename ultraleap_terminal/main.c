//
//  main.c
//  ultraleap_terminal
//
//  Created by Andrew Benson on 10/6/23.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>
#define LockMutex pthread_mutex_lock
#define UnlockMutex pthread_mutex_unlock

#include "LeapC.h"

static LEAP_CONNECTION leapHandle = NULL;
static LEAP_DEVICE_INFO *lastDevice = NULL;
static bool _isRunning = false;
bool IsConnected = false;
static LEAP_TRACKING_EVENT *lastFrame = NULL;
int64_t lastFrameID = 0; //The last frame received

//threading
static pthread_t pollingThread;
static pthread_mutex_t dataLock;

static void* serviceMessageLoop(void * unused);
static void setFrame(const LEAP_TRACKING_EVENT *frame);
static void setDevice(const LEAP_DEVICE_INFO *deviceProps);
const char* ResultString(eLeapRS r);

/** Translates eLeapRS result codes into a human-readable string. */
const char* ResultString(eLeapRS r) {
  switch(r){
    case eLeapRS_Success:                  return "eLeapRS_Success";
    case eLeapRS_UnknownError:             return "eLeapRS_UnknownError";
    case eLeapRS_InvalidArgument:          return "eLeapRS_InvalidArgument";
    case eLeapRS_InsufficientResources:    return "eLeapRS_InsufficientResources";
    case eLeapRS_InsufficientBuffer:       return "eLeapRS_InsufficientBuffer";
    case eLeapRS_Timeout:                  return "eLeapRS_Timeout";
    case eLeapRS_NotConnected:             return "eLeapRS_NotConnected";
    case eLeapRS_HandshakeIncomplete:      return "eLeapRS_HandshakeIncomplete";
    case eLeapRS_BufferSizeOverflow:       return "eLeapRS_BufferSizeOverflow";
    case eLeapRS_ProtocolError:            return "eLeapRS_ProtocolError";
    case eLeapRS_InvalidClientID:          return "eLeapRS_InvalidClientID";
    case eLeapRS_UnexpectedClosed:         return "eLeapRS_UnexpectedClosed";
    case eLeapRS_UnknownImageFrameRequest: return "eLeapRS_UnknownImageFrameRequest";
    case eLeapRS_UnknownTrackingFrameID:   return "eLeapRS_UnknownTrackingFrameID";
    case eLeapRS_RoutineIsNotSeer:         return "eLeapRS_RoutineIsNotSeer";
    case eLeapRS_TimestampTooEarly:        return "eLeapRS_TimestampTooEarly";
    case eLeapRS_ConcurrentPoll:           return "eLeapRS_ConcurrentPoll";
    case eLeapRS_NotAvailable:             return "eLeapRS_NotAvailable";
    case eLeapRS_NotStreaming:             return "eLeapRS_NotStreaming";
    case eLeapRS_CannotOpenDevice:         return "eLeapRS_CannotOpenDevice";
    default:                               return "unknown result type.";
  }
}

LEAP_CONNECTION* OpenConnection(void){
  if(_isRunning){
    return &leapHandle;
  }
  if(leapHandle || LeapCreateConnection(NULL, &leapHandle) == eLeapRS_Success){
    eLeapRS result = LeapOpenConnection(leapHandle);
    if(result == eLeapRS_Success){
      _isRunning = true;
        printf("Is Running \n");
#if defined(_MSC_VER)
      InitializeCriticalSection(&dataLock);
      pollingThread = (HANDLE)_beginthread(serviceMessageLoop, 0, NULL);
#else
      pthread_mutex_init(&dataLock, NULL);
      pthread_create(&pollingThread, NULL, serviceMessageLoop, NULL);
#endif
    }
  }
  return &leapHandle;
}
/**
 * Caches the newest frame by copying the tracking event struct returned by
 * LeapC.
 */
void setFrame(const LEAP_TRACKING_EVENT *frame){
  LockMutex(&dataLock);
  if(!lastFrame) lastFrame = malloc(sizeof(*frame));
  *lastFrame = *frame;
  UnlockMutex(&dataLock);
}

LEAP_TRACKING_EVENT* GetFrame(void){
  LEAP_TRACKING_EVENT *currentFrame;
  LockMutex(&dataLock);
  currentFrame = lastFrame;
  UnlockMutex(&dataLock);
  return currentFrame;
}

LEAP_DEVICE_INFO* GetDeviceProperties(void){
  LEAP_DEVICE_INFO *currentDevice;
  LockMutex(&dataLock);
  currentDevice = lastDevice;
  UnlockMutex(&dataLock);
  return currentDevice;
}
/*
* Called by serviceMessageLoop() when a device event is returned by LeapPollConnection()
* Demonstrates how to access device properties.
*/
static void handleDeviceEvent(const LEAP_DEVICE_EVENT *device_event){
 LEAP_DEVICE deviceHandle;
 //Open device using LEAP_DEVICE_REF from event struct.
 eLeapRS result = LeapOpenDevice(device_event->device, &deviceHandle);
 if(result != eLeapRS_Success){
   printf("Could not open device %s.\n", ResultString(result));
   return;
 }

 //Create a struct to hold the device properties, we have to provide a buffer for the serial string
 LEAP_DEVICE_INFO deviceProperties = { sizeof(deviceProperties) };
 // Start with a length of 1 (pretending we don't know a priori what the length is).
 // Currently device serial numbers are all the same length, but that could change in the future
 deviceProperties.serial_length = 1;
 deviceProperties.serial = malloc(deviceProperties.serial_length);
 //This will fail since the serial buffer is only 1 character long
 // But deviceProperties is updated to contain the required buffer length
 result = LeapGetDeviceInfo(deviceHandle, &deviceProperties);
 if(result == eLeapRS_InsufficientBuffer){
   //try again with correct buffer size
   deviceProperties.serial = realloc(deviceProperties.serial, deviceProperties.serial_length);
   result = LeapGetDeviceInfo(deviceHandle, &deviceProperties);
   if(result != eLeapRS_Success){
     printf("Failed to get device info %s.\n", ResultString(result));
     free(deviceProperties.serial);
     return;
   }
 }
 setDevice(&deviceProperties);
 free(deviceProperties.serial);
 LeapCloseDevice(deviceHandle);
}

/**
 * Caches the last device found by copying the device info struct returned by
 * LeapC.
 */
static void setDevice(const LEAP_DEVICE_INFO *deviceProps){
  LockMutex(&dataLock);
  if(lastDevice){
    free(lastDevice->serial);
  } else {
    lastDevice = malloc(sizeof(*deviceProps));
  }
  *lastDevice = *deviceProps;
  lastDevice->serial = malloc(deviceProps->serial_length);
  memcpy(lastDevice->serial, deviceProps->serial, deviceProps->serial_length);
  UnlockMutex(&dataLock);
}

static void* serviceMessageLoop(void * unused){
  eLeapRS result;
  LEAP_CONNECTION_MESSAGE msg;
  while(_isRunning){
    unsigned int timeout = 1000;
    result = LeapPollConnection(leapHandle, timeout, &msg);

    if(result != eLeapRS_Success){
      //printf("LeapC PollConnection call was %s.\n", ResultString(result));
      continue;
    }
      if (msg.type == eLeapEventType_Tracking){
          setFrame(msg.tracking_event);
      }
      else if (msg.type == eLeapEventType_Device){
          handleDeviceEvent(msg.device_event);
      }
      else if (msg.type == eLeapEventType_Connection){
          IsConnected = true;
      }
  }
    return NULL;
}

/** Cross-platform sleep function */
void millisleep(int milliseconds){
#ifdef _WIN32
    Sleep(milliseconds);
#else
    usleep(milliseconds*1000);
#endif
  }

int main(int argc, const char * argv[]) {
    // insert code here...
    OpenConnection();
    while(!IsConnected) millisleep(100); //wait a bit to let the connection complete
    printf("leap: connected \n");
    LEAP_DEVICE_INFO* deviceProps = GetDeviceProperties();
    if(deviceProps)
      printf("device id %s.\n", deviceProps->serial);
    
    printf("tracking \n");
    for(;;){
      LEAP_TRACKING_EVENT *frame = GetFrame();
      if(frame && (frame->tracking_frame_id > lastFrameID)){
        lastFrameID = frame->tracking_frame_id;
        printf("%i hands.\n", (int)frame->nHands);
        for(uint32_t h = 0; h < frame->nHands; h++){
          LEAP_HAND* hand = &frame->pHands[h];
          printf("%s hand: (%f, %f, %f).\n",
                      (hand->type == eLeapHandType_Left ? "left" : "right"),
                      hand->palm.position.x,
                      hand->palm.position.y,
                      hand->palm.position.z);
        }
      }
    } //ctrl-c to exit
    return 0;
}
