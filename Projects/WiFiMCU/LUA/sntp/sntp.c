/**
******************************************************************************
* @file    sntp.c 
* @author  William Xu
* @version V1.0.0
* @date    05-May-2014
* @brief   Create a NTP client thread, and synchronize RTC with NTP server.
******************************************************************************
*
*  The MIT License
*  Copyright (c) 2014 MXCHIP Inc.
*
*  Permission is hereby granted, free of charge, to any person obtaining a copy 
*  of this software and associated documentation files (the "Software"), to deal
*  in the Software without restriction, including without limitation the rights 
*  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
*  copies of the Software, and to permit persons to whom the Software is furnished
*  to do so, subject to the following conditions:
*
*  The above copyright notice and this permission notice shall be included in
*  all copies or substantial portions of the Software.
*
*  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
*  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
*  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
*  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
*  WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR 
*  IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************
*/
#include <time.h>
#include <locale.h>
#include <stdlib.h> 

#include "mico.h"
#include "SocketUtils.h"

#define UNIX_OFFSET 		 2208988800U // seconds from 01/01/1900 to 01/01/1970
#define NTP_Port                 123
#define NTP_Flags                0xdb 
#define NTP_Stratum              0x0
#define NTP_Poll                 0x0a
#define NTP_Precision            0xfa
#define NTP_Root_Delay           0x8000
#define NTP_Root_Dispersion      0xa00b0000

static uint8_t ntp_time_set = 0;
static uint8_t ntp_thread_started = 0;
static volatile bool _wifiConnected = false;
static mico_semaphore_t  _wifiConnected_sem = NULL;

static int ntp_time_zone = 0;
static bool log = 0;
#define ntp_log(M, ...) if (log == true) printf(M, ##__VA_ARGS__)
//#define ntp_log_trace() custom_log_trace("NTP client")

static char NTP_Server[32]  = {0};

struct NtpPacket
{
	uint8_t flags;
	uint8_t stratum;
	uint8_t poll;
	uint8_t precision;
	uint32_t root_delay;
	uint32_t root_dispersion;
	uint8_t referenceID;
	uint32_t ref_ts_sec;
	uint32_t ref_ts_frac;
	uint32_t origin_ts_sec;
	uint32_t origin_ts_frac;
	uint32_t recv_ts_sec;
	uint32_t recv_ts_frac;
	uint32_t trans_ts_sec;
	uint32_t trans_ts_frac;
};

//--------------------------
uint8_t isNtpTimeSet( void )
{
  return ntp_time_set;
}

//-------------------------------
uint8_t ntpTthreadStarted( void )
{
  return ntp_thread_started;
}

//-----------------------------------------------------------
static void ntpNotify_WifiStatusHandler(int event, void *arg)
{
  //ntp_log_trace();
  UNUSED_PARAMETER(arg);
  switch (event) {
  case NOTIFY_STATION_UP:
    _wifiConnected = true;
    if(_wifiConnected_sem)
      mico_rtos_set_semaphore(&_wifiConnected_sem);
    break;
  case NOTIFY_STATION_DOWN:
    break;
  default:
    break;
  }
  return;
}

//-------------------------------------
static void NTPClient_thread(void *arg)
{
  //ntp_log_trace();
  OSStatus err = kUnknownErr;
  UNUSED_PARAMETER( arg );
  
  int  Ntp_fd = -1;
  fd_set readfds;
  struct timeval_t t ;
  struct sockaddr_t addr;
  socklen_t addrLen;	
  char ipstr[16];
  unsigned int trans_sec, current;
  struct NtpPacket outpacket ,inpacket;
  struct tm *currentTime;
  mico_rtc_time_t time;
  LinkStatusTypeDef wifi_link;
  int contry = 0;
  
  ntp_thread_started = 1;
  
  // Register notifications
  err = mico_system_notify_register( mico_notify_WIFI_STATUS_CHANGED, (void *)ntpNotify_WifiStatusHandler, NULL );
  require_noerr( err, exit ); 
 
  memset(&outpacket,0x0,sizeof(outpacket));
  memset(&inpacket,0x0,sizeof(inpacket));

  outpacket.flags = NTP_Flags;
  outpacket.stratum = NTP_Stratum;
  outpacket.poll = NTP_Poll;
  outpacket.precision = NTP_Precision;
  outpacket.root_delay = NTP_Root_Delay;
  outpacket.root_dispersion = NTP_Root_Dispersion;
  
  err = micoWlanGetLinkStatus( &wifi_link );
  require_noerr( err, exit );

  if( wifi_link.is_connected == true )
    _wifiConnected = true;
  
  if(_wifiConnected == false)
    mico_rtos_get_semaphore(&_wifiConnected_sem, MICO_WAIT_FOREVER);
  
  Ntp_fd = socket(AF_INET, SOCK_DGRM, IPPROTO_UDP);
  require_action(IsValidSocket( Ntp_fd ), exit, err = kNoResourcesErr );
  addr.s_ip = INADDR_ANY; 
  addr.s_port = 45000;
  err = bind(Ntp_fd, &addr, sizeof(addr));
  err = kNoErr;
  require_noerr(err, exit);

   contry=0;
   while(1) {
     err = gethostbyname(NTP_Server, (uint8_t *)ipstr, 16);
     contry+=1;
     if (contry > 4) { require_noerr(err, exit); }
     else { require_noerr(err, ReConnWithDelay); }
     //ntp_log("NTP server address: %s",ipstr);
     break;

   ReConnWithDelay:
     mico_thread_sleep(5);
   }

  addr.s_ip = inet_addr(ipstr);
  addr.s_port = NTP_Port;

  t.tv_sec = 5;
  t.tv_usec = 0;
  
  while(1) {
    require_action(sendto(Ntp_fd, &outpacket,sizeof(outpacket), 0, &addr, sizeof(addr)), exit, err = kNotWritableErr);

    FD_ZERO(&readfds);
    FD_SET(Ntp_fd, &readfds);

    select(1, &readfds, NULL, NULL, &t);
    
    if(FD_ISSET(Ntp_fd, &readfds))
    {
      require_action(recvfrom(Ntp_fd, &inpacket, sizeof(struct NtpPacket), 0, &addr, &addrLen)>=0, exit, err = kNotReadableErr);
      trans_sec = inpacket.trans_ts_sec;
      trans_sec = ntohl(trans_sec);
      current = trans_sec - UNIX_OFFSET + (ntp_time_zone*3600);
      if (log) {
        ntp_log("\r\nTime Synchronised from %s, tz=%d: %s\r\n",NTP_Server,ntp_time_zone,asctime(gmtime(&current)));
      }

      currentTime = gmtime(&current);
      time.sec = currentTime->tm_sec;
      time.min = currentTime->tm_min ;
      time.hr = currentTime->tm_hour;

      time.date = currentTime->tm_mday;
      time.weekday = currentTime->tm_wday;
      time.month = currentTime->tm_mon + 1;
      time.year = (currentTime->tm_year + 1900)%100;
      MicoRtcSetTime( &time );
      ntp_time_set = 1;
      goto exit;
    }
  }
exit:
  if ((log) && (err != kNoErr)) {
    ntp_log("\r\nNTP client exited with error = %d", err);
  }
  mico_system_notify_remove( mico_notify_WIFI_STATUS_CHANGED, (void *)ntpNotify_WifiStatusHandler );
  if(_wifiConnected_sem) mico_rtos_deinit_semaphore(&_wifiConnected_sem);
  SocketClose(&Ntp_fd);
  ntp_thread_started = 0;
  mico_rtos_delete_thread(NULL);
  return;
}

//----------------------------------------------------------------
OSStatus sntp_client_start( int tz, const char *ntpserv, bool lg )
{
  ntp_time_set = 0;
  ntp_time_zone = tz;
  if (ntpserv != NULL) sprintf(NTP_Server, ntpserv);
  else sprintf(NTP_Server, "time1.google.com");
  log = lg;
  
  mico_rtos_init_semaphore(&_wifiConnected_sem, 1);
  return mico_rtos_create_thread(NULL, MICO_APPLICATION_PRIORITY, "NTP Client", NTPClient_thread, STACK_SIZE_NTP_CLIENT_THREAD, NULL );
}
