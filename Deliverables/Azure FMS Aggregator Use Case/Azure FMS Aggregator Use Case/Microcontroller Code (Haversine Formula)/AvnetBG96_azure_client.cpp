// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

//#define USE_MQTT
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdlib.h>


#include "mbed.h"
#ifdef USE_MQTT
#include "iothubtransportmqtt.h"
#else
#include "iothubtransporthttp.h"
#endif
#include "iothub_client_core_common.h"
#include "iothub_client_ll.h"
#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/agenttime.h"
#include "jsondecoder.h"
#include "bg96gps.hpp"
#include "button.hpp"

#define APP_VERSION "1.2"
#define IOT_AGENT_OK CODEFIRST_OK

#include "azure_certs.h"

/* The following is the message we will be sending to Azure */
typedef struct IoTDevice_t {
    char* ObjectName;
    char* ObjectType;
    char* Version;
    char* ReportingDevice;
    float lat;
    float lon;
    float gpstime;
    char  gpsdate[7];
    float Temperature;
    int   Humidity;
    int   Pressure;
    int   Tilt;
    int   ButtonPress;
    char* TOD;
	float totalKM;
	int	  sentMsgs;
    } IoTDevice;

#define IOTDEVICE_MSG_FORMAT       \
   "{"                             \
     "\"ObjectName\":\"%s\","      \
     "\"ObjectType\":\"%s\","      \
     "\"Version\":\"%s\","         \
     "\"ReportingDevice\":\"%s\"," \
     "\"location\":{" \
     "\"lat\":\"%6.7f\","     \
     "\"lon\":\"%6.7f\"},"    \
     "\"GPSTime\":\"%6.0f\","      \
     "\"GPSDate\":\"%s\","         \
     "\"Temperature\":\"%.02f\","  \
     "\"Humidity\":\"%d\","        \
     "\"Pressure\":\"%d\","        \
     "\"Tilt\":\"%d\","            \
     "\"ButtonPress\":\"%d\","     \
     "\"TOD\":\"%s UTC\","          \
	 "\"TotalKMToday\":\"%f\","          \
	 "\"sentMsgs\":\"%d\""			\
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


static const char* connectionString=	"HostName=iotc-000c0e58-161c-4800-b262-e83ffb100c36.azure-devices.net;DeviceId=681ed34c-bdf4-4907-b947-957a8daae597;SharedAccessKey=To5I/JJEJpWqjVZuKfsXkRtUMb46JICdXq8ZUYyYWsI=";
static const char* deviceId = "681ed34c-bdf4-4907-b947-957a8daae597"; /*must match the one on connectionString*/


// to report F uncomment this #define CTOF(x)         (((double)(x)*9/5)+32)
#define CTOF(x)         (x)

Thread azure_client_thread(osPriorityNormal, 8*1024, NULL, "azure_client_thread");
static void azure_task(void);

const int    blink_interval = 500; //msec
int          RED_state, BLUE_state, GREEN_state;


////////////////////////////////////////////////////////////////////////
//////////////////        HAVERSINE IMP        /////////////////////////
////////////////////////////////////////////////////////////////////////
float		 lastLat = 0;
float		 lastLon = 0;
float		 totalKM = 0;
int			 sentMsgs = 0;
float			lasttime;

/* create the GPS elements for example program */
gps_data gdata; 
bg96_gps gps;   

#define GREEN       4  //0 0100 GREEN
#define BLUE        2  //0 0010
#define RED         1  //0 0001 RED





/* Button callbacks for a press and release (light an LED) */
static bool button_pressed = false;
void ub_press(void)
{
    button_pressed = true;
}


////////////////////////////////////////////////////////////////////////
//////////////////        HAVERSINE IMP        /////////////////////////
////////////////////////////////////////////////////////////////////////
long double toRadians(const long double degree)
{
	// cmath library in C++  
	// defines the constant 
	// M_PI as the value of 
	// pi accurate to 1e-30 
	long double one_deg = (M_PI) / 180;
	return (one_deg * degree);
}


////////////////////////////////////////////////////////////////////////
//////////////////        HAVERSINE IMP        /////////////////////////
////////////////////////////////////////////////////////////////////////
void totalKilometersTravelled(IoTDevice* iotDev) {
	lasttime = iotDev->gpstime;

	if (lastLat == 0 && lastLon == 0) {
		lastLat = iotDev->lat;
		lastLon = iotDev->lon;
	}
	float curLat = iotDev->lat;
	float curLon = iotDev->lon;

	float lat1 = toRadians(lastLat);
	float long1 = toRadians(lastLon);
	float lat2 = toRadians(curLat);
	float long2 = toRadians(curLon);
	
	// Haversine Formula 
	long double dlong = long2 - long1;
	long double dlat = lat2 - lat1;

	long double ans = pow(sin(dlat / 2), 2) +
		cos(lat1) * cos(lat2) *
		pow(sin(dlong / 2), 2);
	
	ans = 2 * asin(sqrt(ans));

	// Radius of Earth in  
	// Kilometers, R = 6371 
	// Use R = 3956 for miles 
	long double R = 6371;

	// Calculate the result 
	ans = ans * R;

	totalKM += ans;
	lastLat = curLat;
	lastLon = curLon;
	printf("\nThe total distance travelled so far is: %f KM\n", totalKM);
	if (iotDev->gpstime < lasttime) { // Current time should always be past lasttime, except for one time resets!
		totalKM = 0;
		lasttime = iotDev->gpstime;
		printf("The clock has reset, and totalKM has been reset to 0\n");
	}
}

void ub_release(int x)
{
    button_pressed = false;
}



static int tilt_event;

void mems_int1(void)
{
    tilt_event++;
}

void mems_init(void)
{
#if MBED_CONF_APP_IKSVERSION == 2
    acc_gyro->attach_int1_irq(&mems_int1);  // Attach callback to LSM6DSL INT1
    hum_temp->enable();                     // Enable HTS221 enviromental sensor
    pressure->enable();                     // Enable barametric pressure sensor
    acc_gyro->enable_x();                   // Enable LSM6DSL accelerometer
    acc_gyro->enable_tilt_detection();      // Enable Tilt Detection
#endif
}

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
    printf("->using %s Environmental Sensor\r\n", ENV_SENSOR);
#ifdef IOTHUBTRANSPORTHTTP_H
    printf("->using HTTPS Transport Protocol\r\n");
#else
    printf("->using MQTT Transport Protocol\r\n");
#endif
    printf("\r\n");

    if (platform_init() != 0) {
       printf("Error initializing the platform\r\n");
       return -1;
       }

    printf("[ start GPS ] ");
    gps.gpsPower(true);
    printf("Successful.\r\n[get GPS loc] ");
    fflush(stdout);
    gps.gpsLocation(&gdata);
    printf("Latitude = %6.3f Longitude = %6.3f date=%s, time=%6.0f\n\n",gdata.lat,gdata.lon,gdata.date,gdata.utc);

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

    mems_init();
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
char* makeMessage(IoTDevice* iotDev)
{
    static char buffer[80];
    const int   msg_size = 512;
    char*       ptr      = (char*)malloc(msg_size);
    time_t      rawtime;
    struct tm   *ptm;
  


////////////////////////////////////////////////////////////////////////
//////////////////        HAVERSINE IMP        /////////////////////////
////////////////////////////////////////////////////////////////////////
	totalKilometersTravelled(iotDev);
	sentMsgs++;
	iotDev->sentMsgs = sentMsgs;


    time(&rawtime);
    ptm = gmtime(&rawtime);
    strftime(buffer,80,"%a %F %X",ptm);
    iotDev->TOD = buffer;
    int c = (strstr(buffer,":")-buffer) - 2;
    printf("Send IoTHubClient Message@%s - ",&buffer[c]);
    snprintf(ptr, msg_size, IOTDEVICE_MSG_FORMAT,
                            iotDev->ObjectName,
                            iotDev->ObjectType,
                            iotDev->Version,
                            iotDev->ReportingDevice,
                            iotDev->lat,
                            iotDev->lon,
                            iotDev->gpstime,
                            iotDev->gpsdate,
                            iotDev->Temperature,
                            iotDev->Humidity,
                            iotDev->Pressure,
                            iotDev->Tilt,
                            iotDev->ButtonPress,
                            iotDev->TOD,
							iotDev->totalKM,
							iotDev->sentMsgs);
	printf(ptr);

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

    free(temp);

    return IOTHUBMESSAGE_ACCEPTED;
}

void azure_task(void)
{
    bool button_press = false, runTest = true;
    bool tilt_detection_enabled=true;
    float gtemp, ghumid, gpress;

    int  k;
    int  msg_sent=1;

    Button user_button(BUTTON1,Button::MBED_CONF_APP_BUTTON_ACTIVE_STATE, ub_release);
    user_button.setButton_press_cb(ub_press);

    /* Setup IoTHub client configuration */
#ifdef IOTHUBTRANSPORTHTTP_H
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, HTTP_Protocol);
#else
    IOTHUB_CLIENT_LL_HANDLE iotHubClientHandle = IoTHubClient_LL_CreateFromConnectionString(connectionString, MQTT_Protocol);
#endif

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

#ifdef IOTHUBTRANSPORTHTTP_H
    // polls will happen effectively at ~10 seconds.  The default value of minimumPollingTime is 25 minutes. 
    // For more information, see:
    //     https://azure.microsoft.com/documentation/articles/iot-hub-devguide/#messaging

    unsigned int minimumPollingTime = 9;
    if (IoTHubClient_LL_SetOption(iotHubClientHandle, "MinimumPollingTime", &minimumPollingTime) != IOTHUB_CLIENT_OK)
        printf("failure to set option \"MinimumPollingTime\"\r\n");
#endif

    IoTDevice* iotDev = (IoTDevice*)malloc(sizeof(IoTDevice));
    if (iotDev == NULL) {
        printf("Failed to malloc space for IoTDevice\r\n");
        return;
        }

    // set C2D and device method callback
    IoTHubClient_LL_SetMessageCallback(iotHubClientHandle, receiveMessageCallback, NULL);

    //
    // setup the iotDev struction contents...
    //
    iotDev->ObjectName      = (char*)"NUCLEOBOARD2";
    iotDev->ObjectType      = (char*)"SensorData";
    iotDev->Version         = (char*)APP_VERSION;
    iotDev->ReportingDevice = (char*)"STL496ZG-BG96";
    iotDev->TOD             = (char*)"";
    iotDev->Temperature     = 0.0;
    iotDev->lat             = 0.0;
    iotDev->lon             = 0.0;
    iotDev->gpstime         = 0.0;
    iotDev->Humidity        = 0;
    iotDev->Pressure        = 0;
    iotDev->Tilt            = 0x2;
    iotDev->ButtonPress     = 0;
    memset(iotDev->gpsdate,0x00,7);
	iotDev->totalKM = 0.0;
	iotDev->sentMsgs = 0;


    while (runTest) {
        char*  msg;
        size_t msgSize;

        gps.gpsLocation(&gdata);
        iotDev->lat = gdata.lat;
        iotDev->lon = gdata.lon;
        iotDev->gpstime = gdata.utc;
        memcpy(iotDev->gpsdate, gdata.date, 7);
		iotDev->totalKM = totalKM;

#if MBED_CONF_APP_IKSVERSION == 2
        hum_temp->get_temperature(&gtemp);           // get Temp
        hum_temp->get_humidity(&ghumid);             // get Humidity
        pressure->get_pressure(&gpress);             // get pressure
#elif MBED_CONF_APP_IKSVERSION == 1
        CALL_METH(temp, get_temperature, &gtemp, 0.0f);
        CALL_METH(hum, get_humidity, &ghumid, 0.0f);
        CALL_METH(pressure, get_pressure, &gpress, 0.0f);
#else
        gtemp  = 0.0;
        ghumid = 0.0;
        gpress = 0.0;
#endif

        iotDev->Temperature = CTOF(gtemp);
        iotDev->Humidity    = (int)ghumid;
        iotDev->Pressure    = (int)gpress;

        if( tilt_event ) {
            tilt_event = 0;
            iotDev->Tilt |= 1;
            }

        iotDev->ButtonPress = button_pressed?1:0;
        if( user_button.chkButton_press(&k) ) {
            if( k > 3000 ) {
                printf("User Requested Termination (held button for %d msec), exiting.\n",k);
                runTest = false;
                }
            else{
                iotDev->ButtonPress = 1;
                tilt_detection_enabled = !tilt_detection_enabled;
#if MBED_CONF_APP_IKSVERSION == 2
                if( !tilt_detection_enabled ) {
                    acc_gyro->disable_tilt_detection();
                    iotDev->Tilt &= 1;
                    }
                else{
                    acc_gyro->enable_tilt_detection();
                    iotDev->Tilt |= 2;
                    }
#endif
                }
            }

        printf("(%04d)",msg_sent++);
        msg = makeMessage(iotDev);
        msgSize = strlen(msg);
        sendMessage(iotHubClientHandle, msg, msgSize);
        free(msg);
        iotDev->Tilt &= 0x2;
        iotDev->ButtonPress = 0;

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
        ThisThread::sleep_for(3000);  //in msec
        }
    free(iotDev);
    IoTHubClient_LL_Destroy(iotHubClientHandle);
    return;
}

