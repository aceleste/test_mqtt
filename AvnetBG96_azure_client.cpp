// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#include "mbed.h"
#include "iothubtransporthttp.h"
#include "iothub_client_core_common.h"
#include "iothub_client_ll.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/agenttime.h"
#include "jsondecoder.h"

#define APP_VERSION "1.0"
#define IOT_AGENT_OK CODEFIRST_OK

#include "azure_certs.h"

typedef struct Position_t {
    float latitude;
    float longitude;
    int    geofence;
} Position;

typedef struct Temperature_t {
    float containerTemperature;
    float heaterTemperature;
} Temperature;

typedef struct Health_t {
    float  batteryVoltage;
    char* network;
    int    signalStrength;
} Health;

/* The following is the message we will be sending to Azure */
typedef struct IoTDeviceToSystem_t {
    char* timestamp;
    char* device;
    Position* position;
    Temperature* temperature;
    Health* health;
    } IoTDeviceToSystem;

#define IOTDEVICE_MSG_FORMAT       \
   "{"                             \
     "\"timestamp\":\"%s\","       \
     "\"device\":\"%s\","          \
     "\"position\": {"             \
     "\"latitude\":%.4f,"          \
     "\"longitude\":%.4f,"         \
     "\"geoFence\":%d"             \
     "},"                          \
     "\"temperature\": {"          \
     "\"container\":%.1f,"         \
     "\"heater\":%.1f"             \
     "},"                          \
     "\"health\": {"               \
     "\"batteryVoltage\":%.2f,"    \
     "\"network\":\"%s\","         \
     "\"signalStrength\":%d"     \
     "}"                           \
   "}"                             

/* initialize the expansion board && sensors */

#if MBED_CONF_APP_IKSVERSION == 2
  #define ENV_SENSOR "IKS01A2"
  #include "XNucleoIKS01A2.h"
  static HTS221Sensor   *hum_temp;
  static LSM6DSLSensor  *acc_gyro;
  static LPS22HBSensor  *pressure;
#elif MBED_CONF_APP_IKSVERSION == 1
  #define ENV_SENSOR "IKS01A1"
  #include "x_nucleo_iks01a1.h"
  static HumiditySensor *hum;
  static TempSensor     *temp;
  static PressureSensor *pressure;
  static GyroSensor     *acc_gyro;
#else
  #define ENV_SENSOR "NO"
#endif

static const char* connectionString = "HostName=BTL-IOT-Hub.azure-devices.net;DeviceId=Test-Device-1;SharedAccessKey=BTL-IOT-Hub.azure-devices.net%2Fdevices%2FTest-Device-1&sig=jMZ6thMEEQcVnypMqEAMFOVfrv5pDUjvJBERxPy1rus%3D&se=1566668939";

//static const char* deviceId         = "Test-Device-1"; /*must match the one on connectionString*/

#define CTOF(x)         (((double)(x)*9/5)+32)

Thread azure_client_thread(osPriorityNormal, 8*1024, NULL, "azure_client_thread");
static void azure_task(void);

//
// The main routine simply prints a banner, initializes the system
// starts the worker threads and waits for a termination (join)

int main(void)
{
    printf("\r\n");
    printf("     ****\r\n");
    printf("    **  **     Azure IoTClient Example, version %s\r\n", APP_VERSION);
    printf("   **    **    by AVNET\r\n");
    printf("  ** ==== **   \r\n");
    printf("\r\n");
    printf("The example program interacts with Azure IoTHub sending \r\n");
    printf("sensor data and receiving messeages (using ARM Mbed OS v5.x)\r\n");
    printf("[using %s Environmental Sensor]\r\n", ENV_SENSOR);
    printf("\r\n");

    if (platform_init() != 0) {
       printf("Error initializing the platform\r\n");
       return -1;
       }

#if MBED_CONF_APP_IKSVERSION == 2
  XNucleoIKS01A2 *mems_expansion_board = XNucleoIKS01A2::instance(I2C_SDA, I2C_SCL, D4, D5);
  hum_temp = mems_expansion_board->ht_sensor;
  acc_gyro = mems_expansion_board->acc_gyro;
  pressure = mems_expansion_board->pt_sensor;
#elif MBED_CONF_APP_IKSVERSION == 1
  X_NUCLEO_IKS01A1 *mems_expansion_board = X_NUCLEO_IKS01A1::Instance(I2C_SDA, I2C_SCL);
  hum      = mems_expansion_board->ht_sensor;
  temp     = mems_expansion_board->ht_sensor;
  pressure = mems_expansion_board->pt_sensor;
  acc_gyro = mems_expansion_board->GetGyroscope();
#endif

    azure_client_thread.start(azure_task);

    azure_client_thread.join();

    platform_deinit();
    printf(" - - - - - - - ALL DONE - - - - - - - \n");
    return 0;
}

//
// This function sends the actual message to azure
//

// *************************************************************
//  AZURE STUFF...
//
char* makeMessage(IoTDeviceToSystem* iotDev)
{
    static char buffer[80];
    const int   msg_size = 512;
    char*       ptr      = (char*)malloc(msg_size);
    time_t      rawtime;
    struct tm   *ptm;
  
    time(&rawtime);
    ptm = gmtime(&rawtime);
    strftime(buffer,80,"%a %F %X",ptm);
    iotDev->timestamp = buffer;
    int c = (strstr(buffer,":")-buffer) - 2;
    printf("Send IoTHubClient Message@%s - ",&buffer[c]);
    snprintf(ptr, msg_size, IOTDEVICE_MSG_FORMAT,
                            iotDev->timestamp,
                            iotDev->device,
                            iotDev->position->latitude,
                            iotDev->position->longitude,
                            iotDev->position->geofence,
                            iotDev->temperature->containerTemperature,
                            iotDev->temperature->heaterTemperature,
                            iotDev->health->batteryVoltage,
                            iotDev->health->network,
                            iotDev->health->signalStrength
                            );
    return ptr;
}

void sendMessage(IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle, char* buffer, size_t size)
{
    IOTHUB_MESSAGE_HANDLE messageHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)buffer, size);
    if (messageHandle == NULL) {
        printf("unable to create a new IoTHubMessage\r\n");
        return;
        }
    if (IoTHubClient_LL_SendEventAsync(iotHubClientHandle, messageHandle, NULL, NULL) != IOTHUB_CLIENT_OK)
        printf("FAILED to send! [RSSI=%d]\n", platform_RSSI());
    else
        printf("OK. [RSSI=%d]\n",platform_RSSI());

    IoTHubMessage_Destroy(messageHandle);
}

IOTHUBMESSAGE_DISPOSITION_RESULT receiveMessageCallback(
    IOTHUB_MESSAGE_HANDLE message, 
    void *userContextCallback)
{
    const unsigned char *buffer = NULL;
    size_t size = 0;

    if (IOTHUB_MESSAGE_OK != IoTHubMessage_GetByteArray(message, &buffer, &size))
    {
        return IOTHUBMESSAGE_ABANDONED;
    }

    // message needs to be converted to zero terminated string
    char * temp = (char *)malloc(size + 1);
    if (temp == NULL)
    {
        return IOTHUBMESSAGE_ABANDONED;
    }
    strncpy(temp, (char*)buffer, size);
    temp[size] = '\0';

    printf("Receiving message: '%s'\r\n", temp);

    free(temp);

    return IOTHUBMESSAGE_ACCEPTED;
}

void azure_task(void)
{
    int  msg_sent=1;

    /* Setup IoTHub client configuration */
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, HTTP_Protocol);
    if (iotHubClientHandle == NULL) {
        printf("Failed on IoTHubClient_Create\r\n");
        return;
        }

    // add the certificate information
    if (IoTHubClient_LL_SetOption(iotHubClientHandle, "TrustedCerts", certificates) != IOTHUB_CLIENT_OK)
        printf("failure to set option \"TrustedCerts\"\r\n");

#if MBED_CONF_APP_TELUSKIT == 1
    if (IoTHubClient_LL_SetOption(iotHubClientHandle, "product_info", "TELUSIOTKIT") != IOTHUB_CLIENT_OK)
        printf("failure to set option \"product_info\"\r\n");
#endif

    // polls will happen effectively at ~10 seconds.  The default value of minimumPollingTime is 25 minutes. 
    // For more information, see:
    //     https://azure.microsoft.com/documentation/articles/iot-hub-devguide/#messaging

    unsigned int minimumPollingTime = 9;
    if (IoTHubClient_LL_SetOption(iotHubClientHandle, "MinimumPollingTime", &minimumPollingTime) != IOTHUB_CLIENT_OK)
        printf("failure to set option \"MinimumPollingTime\"\r\n");


    Position* iotPos = (Position*)malloc(sizeof(Position));
    if (iotPos == NULL) {
        printf("Failed to malloc space for Position\r\n");
        return;
    }

    Temperature* iotTemp = (Temperature*)malloc(sizeof(Temperature));
    if (iotTemp == NULL) {
        printf("Failed to malloc space for Temperature\r\n");
        return;
    }

    Health* iotHealth = (Health*)malloc(sizeof(Health));
    if (iotHealth == NULL) {
        printf("Failed to malloc space for Health\r\n");
        return;
    }

    IoTDeviceToSystem* iotDev = (IoTDeviceToSystem*)malloc(sizeof(IoTDeviceToSystem));
    if (iotDev == NULL) {
        printf("Failed to malloc space for IoTDevice\r\n");
        return;
    }

    // set C2D and device method callback
    IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, receiveMessageCallback, NULL);

    // setup the iotPos structure contents...
    iotPos->latitude = 38.898556;
    iotPos->longitude = -77.037852;
    iotPos->geofence = 0;

    // setup the iotTemp structure contents ...
    iotTemp->containerTemperature = 35.4;
    iotTemp->heaterTemperature = 37.1;

    // setup the iotHealth structure contents ...
    iotHealth->batteryVoltage = 3.4;
    iotHealth->network = (char*)"Orange";
    iotHealth->signalStrength = 7;
    //
    // setup the iotDev struction contents...
    //
    iotDev->timestamp      = NULL;
    iotDev->device         = (char*)"TEST-DEVICE-1";

    while (1) {
        char*  msg;
        size_t msgSize;

        printf("(%04d)",msg_sent++);
        msg = makeMessage(iotDev);
        msgSize = strlen(msg);
        sendMessage(iotHubClientHandle, msg, msgSize);
        free(msg);

        /* schedule IoTHubClient to send events/receive commands */
        IoTHubClient_LL_DoWork(iotHubClientHandle);

#if defined(MBED_HEAP_STATS_ENABLED)
        mbed_stats_heap_t heap_stats; //jmf

        mbed_stats_heap_get(&heap_stats);
        printf("  Current heap: %lu\r\n", heap_stats.current_size);
        printf(" Max heap size: %lu\r\n", heap_stats.max_size);
        printf("     alloc_cnt:	%lu\r\n", heap_stats.alloc_cnt);
        printf("alloc_fail_cnt:	%lu\r\n", heap_stats.alloc_fail_cnt);
        printf("    total_size:	%lu\r\n", heap_stats.total_size);
        printf(" reserved_size:	%lu\r\n", heap_stats.reserved_size);
#endif 

#if defined(MBED_STACK_STATS_ENABLED)
        int cnt_ss = osThreadGetCount();
        mbed_stats_stack_t *stats_ss = (mbed_stats_stack_t*) malloc(cnt_ss * sizeof(mbed_stats_stack_t));
        
        cnt_ss = mbed_stats_stack_get_each(stats_ss, cnt_ss);
        for (int i = 0; i < cnt_ss; i++) 
            printf("Thread: 0x%lX, Stack size: %lu, Max stack: %lu\r\n", stats_ss[i].thread_id, stats_ss[i].reserved_size, stats_ss[i].max_size);
#endif 

#if defined(MBED_THREAD_STATS_ENABLED)
#define MAX_THREAD_STATS  10
            mbed_stats_thread_t *stats = new mbed_stats_thread_t[MAX_THREAD_STATS];
            int count = mbed_stats_thread_get_each(stats, MAX_THREAD_STATS);
            
            for(int i = 0; i < count; i++) {
                printf("ID: 0x%lx \n", stats[i].id);
                printf("Name: %s \n", stats[i].name);
                printf("State: %ld \n", stats[i].state);
                printf("Priority: %ld \n", stats[i].priority);
                printf("Stack Size: %ld \n", stats[i].stack_size);
                printf("Stack Space: %ld \n", stats[i].stack_space);
                printf("\n");
                }
#endif 
        Thread::wait(5000);  //in msec
        }
    free(iotDev);
    IoTHubClient_LL_Destroy(iotHubClientHandle);
    return;
}

